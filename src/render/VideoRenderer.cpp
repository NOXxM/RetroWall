#include "render/VideoRenderer.hpp"

#include <d3dcompiler.h>

#include "win32/Log.hpp"

#include <array>
#include <string_view>

namespace lwe::render {

using win32::ThrowIfFailed;
using win32::ThrowIfInvalidHandle;

namespace {

constexpr UINT kBufferCount = 2;
constexpr DXGI_FORMAT kSwapFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

// NV12 -> RGB shader (see render/NV12ToRGB.hlsl for the annotated copy).
constexpr std::string_view kShaderSource = R"HLSL(
Texture2D<float>  LumaPlane   : register(t0);
Texture2D<float2> ChromaPlane : register(t1);
SamplerState      LinearClamp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Live color-grade constants (see VideoRenderer::PostParams).
cbuffer Post : register(b0) {
    float uBrightness;
    float uContrast;
    float uSaturation;
    float uGamma;
    float3 uTint;
    float uTemperature;
    float uBlackout;
    float3 _pad;
};

float3 ApplyPost(float3 c) {
    // Color temperature: warm (<0) lifts red / drops blue; cool (>0) the reverse.
    c.r *= 1.0 - uTemperature * 0.2;
    c.b *= 1.0 + uTemperature * 0.2;
    c *= uBrightness;                                   // brightness
    c = (c - 0.5) * uContrast + 0.5;                    // contrast about mid-gray
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));  // Rec.709 luma
    c = lerp(float3(l, l, l), c, uSaturation);          // saturation
    c *= uTint;                                          // per-channel tint
    c = pow(saturate(c), 1.0 / max(uGamma, 0.01));       // gamma
    return saturate(c);
}

float4 PSMain(VSOut i) : SV_Target {
    if (uBlackout > 0.5) return float4(0.0, 0.0, 0.0, 1.0);  // privacy blackout
    float  y    = LumaPlane.Sample(LinearClamp, i.uv).r;
    float2 cbcr = ChromaPlane.Sample(LinearClamp, i.uv).rg;
    y  = (y - 16.0 / 255.0) * (255.0 / 219.0);
    float cb = (cbcr.x - 128.0 / 255.0) * (255.0 / 224.0);
    float cr = (cbcr.y - 128.0 / 255.0) * (255.0 / 224.0);
    float3 rgb;
    rgb.r = y + 1.5748 * cr;
    rgb.g = y - 0.1873 * cb - 0.4681 * cr;
    rgb.b = y + 1.8556 * cb;
    return float4(ApplyPost(saturate(rgb)), 1.0);
}
)HLSL";

ComPtr<ID3DBlob> Compile(std::string_view src, const char* entry,
                         const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = ::D3DCompile(src.data(), src.size(), nullptr, nullptr,
                                    nullptr, entry, target, flags, 0,
                                    code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        if (errors) {
            ::OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        }
        ThrowIfFailed(hr, "D3DCompile");
    }
    return code;
}

}  // namespace

VideoRenderer::VideoRenderer(HWND target) : target_(target) {
    RECT rc{};
    win32::ThrowIfFalse(::GetClientRect(target, &rc), "GetClientRect(target)");
    width_ = static_cast<std::uint32_t>((rc.right - rc.left) > 0 ? rc.right - rc.left : 1);
    height_ = static_cast<std::uint32_t>((rc.bottom - rc.top) > 0 ? rc.bottom - rc.top : 1);

    CreateDeviceAndSwapChain();
    CreatePipeline();
}

VideoRenderer::~VideoRenderer() {
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
}

// ---------------------------------------------------------------------------
// Device (video-capable, multithread-protected) + composition swapchain
// ---------------------------------------------------------------------------
void VideoRenderer::CreateDeviceAndSwapChain() {
    // VIDEO_SUPPORT enables the DXVA/hardware decoder path. We must NOT use
    // SINGLETHREADED here: Media Foundation's decoder threads touch the shared
    // device, so we enable multithread protection instead (below).
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    constexpr std::array kLevels = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    ComPtr<ID3D11Device> baseDevice;
    ComPtr<ID3D11DeviceContext> baseContext;
    D3D_FEATURE_LEVEL obtained{};

    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, kLevels.data(),
        static_cast<UINT>(kLevels.size()), D3D11_SDK_VERSION,
        baseDevice.GetAddressOf(), &obtained, baseContext.GetAddressOf());
#ifndef NDEBUG
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, kLevels.data(),
            static_cast<UINT>(kLevels.size()), D3D11_SDK_VERSION,
            baseDevice.GetAddressOf(), &obtained, baseContext.GetAddressOf());
    }
#endif
    bool usedWarp = false;
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        // WARP still exposes a (software) video device, so the pipeline builds.
        usedWarp = true;
        hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, kLevels.data(),
            static_cast<UINT>(kLevels.size()), D3D11_SDK_VERSION,
            baseDevice.GetAddressOf(), &obtained, baseContext.GetAddressOf());
    }
    ThrowIfFailed(hr, "D3D11CreateDevice(video)");
    log::Writef(L"D3D device: driver=%s", usedWarp ? L"WARP(software)" : L"hardware");

    device_ = baseDevice;
    context_ = baseContext;

    // Required when the device is shared with Media Foundation.
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ThrowIfFailed(device_.As(&dxgiDevice), "QueryInterface(IDXGIDevice)");
    ComPtr<IDXGIAdapter> adapter;
    ThrowIfFailed(dxgiDevice->GetAdapter(adapter.GetAddressOf()),
                  "IDXGIDevice::GetAdapter");
    ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(factory_.GetAddressOf())),
                  "IDXGIAdapter::GetParent(IDXGIFactory2)");

    // Bind the swapchain DIRECTLY to the WorkerW child window. First try the
    // flip model (keeps the frame-latency waitable); if that is rejected for
    // this child window, fall back to the older BitBlt model, which is the most
    // compatible with child windows. Either way the video draws onto the
    // wallpaper layer with no DirectComposition involved.
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = kSwapFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;  // required for HWND swapchains
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory_->CreateSwapChainForHwnd(
        device_.Get(), target_, &desc, nullptr, nullptr,
        swapChain1.GetAddressOf());

    log::Writef(L"CreateSwapChainForHwnd(flip) hr=0x%08X on hwnd=%p %ux%u",
                static_cast<unsigned>(hr), reinterpret_cast<void*>(target_),
                width_, height_);
    if (SUCCEEDED(hr)) {
        ThrowIfFailed(swapChain1.As(&swapChain_),
                      "QueryInterface(IDXGISwapChain2)");
        swapChain_->SetMaximumFrameLatency(1);
        frameLatencyWaitable_.reset(swapChain_->GetFrameLatencyWaitableObject());
    } else {
        // BitBlt fallback: no flip, no frame-latency waitable (the FramePacer
        // provides timing; the engine skips the waitable wait when it is null).
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = 0;
        ThrowIfFailed(factory_->CreateSwapChainForHwnd(
                          device_.Get(), target_, &desc, nullptr, nullptr,
                          swapChain1.ReleaseAndGetAddressOf()),
                      "CreateSwapChainForHwnd(bitblt)");
        ThrowIfFailed(swapChain1.As(&swapChain_),
                      "QueryInterface(IDXGISwapChain2)");
        log::Write(L"Swapchain: using BitBlt fallback (flip rejected)");
    }

    // We manage desktop layering; stop DXGI trapping Alt+Enter on our window.
    factory_->MakeWindowAssociation(target_, DXGI_MWA_NO_ALT_ENTER);
}

// ---------------------------------------------------------------------------
// NV12 -> RGB pipeline objects
// ---------------------------------------------------------------------------
void VideoRenderer::CreatePipeline() {
    const ComPtr<ID3DBlob> vsBlob = Compile(kShaderSource, "VSMain", "vs_5_0");
    const ComPtr<ID3DBlob> psBlob = Compile(kShaderSource, "PSMain", "ps_5_0");

    ThrowIfFailed(device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                              vsBlob->GetBufferSize(), nullptr,
                                              vs_.GetAddressOf()),
                  "CreateVertexShader");
    ThrowIfFailed(device_->CreatePixelShader(psBlob->GetBufferPointer(),
                                             psBlob->GetBufferSize(), nullptr,
                                             ps_.GetAddressOf()),
                  "CreatePixelShader");

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&sd, sampler_.GetAddressOf()),
                  "CreateSamplerState");

    // Color-grade constant buffer (b0), seeded with identity params.
    const float identity[12] = {1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(identity);  // 48 bytes (multiple of 16)
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbInit{};
    cbInit.pSysMem = identity;
    ThrowIfFailed(device_->CreateBuffer(&cbd, &cbInit, postCb_.GetAddressOf()),
                  "CreateBuffer(post)");
}

void VideoRenderer::SetPostParams(const PostParams& p) {
    if (!postCb_) return;
    // Must match the HLSL cbuffer Post layout exactly.
    const float cb[12] = {p.brightness, p.contrast, p.saturation, p.gamma,
                          p.tintR, p.tintG, p.tintB, p.temperature,
                          p.blackout, 0.0f, 0.0f, 0.0f};
    context_->UpdateSubresource(postCb_.Get(), 0, nullptr, cb, 0, 0);
}

// Create (or recreate) the shader-readable NV12 texture and its two plane SRVs.
void VideoRenderer::EnsureNV12Intermediate(UINT w, UINT h) {
    if (nv12_ && w == nv12W_ && h == nv12H_) {
        return;
    }
    nv12_.Reset();
    lumaSrv_.Reset();
    chromaSrv_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(device_->CreateTexture2D(&desc, nullptr, nv12_.GetAddressOf()),
                  "CreateTexture2D(NV12 intermediate)");

    // For NV12 the SRV plane is selected by the view format:
    //   R8_UNORM   -> luma (Y) plane
    //   R8G8_UNORM -> chroma (CbCr) plane, half resolution
    D3D11_SHADER_RESOURCE_VIEW_DESC luma{};
    luma.Format = DXGI_FORMAT_R8_UNORM;
    luma.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    luma.Texture2D.MipLevels = 1;
    ThrowIfFailed(device_->CreateShaderResourceView(nv12_.Get(), &luma,
                                                    lumaSrv_.GetAddressOf()),
                  "CreateShaderResourceView(luma)");

    D3D11_SHADER_RESOURCE_VIEW_DESC chroma{};
    chroma.Format = DXGI_FORMAT_R8G8_UNORM;
    chroma.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    chroma.Texture2D.MipLevels = 1;
    ThrowIfFailed(device_->CreateShaderResourceView(nv12_.Get(), &chroma,
                                                    chromaSrv_.GetAddressOf()),
                  "CreateShaderResourceView(chroma)");

    nv12W_ = w;
    nv12H_ = h;
}

// ---------------------------------------------------------------------------
// Present one decoded frame
// ---------------------------------------------------------------------------
void VideoRenderer::PresentFrame(ID3D11Texture2D* nv12, UINT subresource,
                                 UINT frameW, UINT frameH) {
    if (nv12 == nullptr || frameW == 0 || frameH == 0) {
        return;
    }
    EnsureNV12Intermediate(frameW, frameH);

    // GPU->GPU copy of the decoder's output slice into our shader-readable NV12
    // texture. Nothing is mapped/read back to system memory. The box is in luma
    // pixels (even bounds for NV12).
    const D3D11_BOX box{0, 0, 0, frameW, frameH, 1};
    context_->CopySubresourceRegion(nv12_.Get(), 0, 0, 0, 0, nv12, subresource,
                                    &box);

    // Backbuffer RTV (flip model: fetch buffer 0 each frame).
    ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())),
                  "GetBuffer(0)");
    ComPtr<ID3D11RenderTargetView> rtv;
    ThrowIfFailed(device_->CreateRenderTargetView(backBuffer.Get(), nullptr,
                                                  rtv.GetAddressOf()),
                  "CreateRenderTargetView(backbuffer)");

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
    context_->OMSetRenderTargets(1, rtvs, nullptr);

    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[] = {lumaSrv_.Get(), chromaSrv_.Get()};
    context_->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    context_->PSSetSamplers(0, 1, samplers);
    ID3D11Buffer* cbs[] = {postCb_.Get()};
    context_->PSSetConstantBuffers(0, 1, cbs);  // b0: color-grade params

    context_->Draw(3, 0);

    // Unbind SRVs so the intermediate can be a copy dest again next frame
    // without a read/write-hazard warning.
    ID3D11ShaderResourceView* nullSrvs[] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, nullSrvs);

    // Sync interval 0: the FramePacer already gated us to the video's cadence,
    // so present immediately and let DWM composite at its own refresh.
    DXGI_PRESENT_PARAMETERS params{};
    ThrowIfFailed(swapChain_->Present1(0, 0, &params), "Present1(video)");
}

void VideoRenderer::Resize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    ThrowIfFailed(
        swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                  DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
        "ResizeBuffers");
    width_ = width;
    height_ = height;
}

}  // namespace lwe::render
