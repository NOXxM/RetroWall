#pragma once
// ---------------------------------------------------------------------------
// render/VideoRenderer.hpp
// Owns the D3D11 device + a DXGI swapchain bound directly to the WorkerW child
// window (HWND swapchain), plus the NV12->RGB pipeline that samples a hardware-
// decoded video frame straight from VRAM into the swapchain.
//
// NOTE: earlier revisions used a DirectComposition composition swapchain, but on
// some Windows builds the composed output did not appear on the wallpaper layer
// when the target is a child of Explorer's WorkerW. A direct HWND swapchain
// (flip-model, with a BitBlt fallback) is the robust, widely-used approach for
// the WorkerW wallpaper technique, so that is what we use.
//
// The device is created for video (BGRA_SUPPORT | VIDEO_SUPPORT, multithread-
// protected, NOT SINGLETHREADED) so it can be shared with Media Foundation via
// an IMFDXGIDeviceManager. Device() is handed to the VideoPlayer for that.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "win32/Error.hpp"

namespace lwe::render {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

class VideoRenderer {
public:
    explicit VideoRenderer(HWND target);
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    // Live post-processing parameters applied in the pixel shader. Defaults are
    // the identity (no visible change).
    struct PostParams {
        float brightness = 1.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float gamma = 1.0f;
        float tintR = 1.0f;
        float tintG = 1.0f;
        float tintB = 1.0f;
        float temperature = 0.0f;  // -1 warm .. +1 cool
        float blackout = 0.0f;     // 1 => render solid black (privacy)
        int   aspectMode = 0;      // 0 Fill (cover), 1 Fit (letterbox), 2 Stretch
    };
    // Stash color/aspect params (call from the render thread before Present).
    // Aspect is resolved against the frame + surface size inside PresentFrame.
    void SetPostParams(const PostParams& params) { post_ = params; }

    // A monitor's rectangle within the (spanning) surface, in surface pixels.
    struct MonitorRect { int x = 0, y = 0, w = 0, h = 0; };
    // Multi-monitor layout: mode 1 (Stretch) or <2 rects => one spanning draw;
    // otherwise the clip is drawn once per rect (Per-Monitor / Clone).
    void SetMonitorLayout(int layoutMode, std::vector<MonitorRect> rects) {
        layoutMode_ = layoutMode;
        monitorRects_ = std::move(rects);
    }

    // Shared with Media Foundation's IMFDXGIDeviceManager.
    [[nodiscard]] ID3D11Device* Device() const noexcept { return device_.Get(); }

    [[nodiscard]] HANDLE FrameLatencyWaitable() const noexcept {
        return frameLatencyWaitable_.get();
    }

    // Convert one hardware-decoded NV12 frame to RGB and present it. `nv12` is
    // the decoder's output texture (possibly a texture-array); `subresource`
    // selects the slice. The pixel data never leaves VRAM. `frameW/frameH` are
    // the coded frame dimensions.
    void PresentFrame(ID3D11Texture2D* nv12, UINT subresource, UINT frameW,
                      UINT frameH);

    void Resize(std::uint32_t width, std::uint32_t height);

private:
    void CreateDeviceAndSwapChain();
    void CreatePipeline();                 // shaders + sampler
    void EnsureNV12Intermediate(UINT w, UINT h);  // lazy, once per size
    void UploadPostCb(UINT frameW, UINT frameH, float surfW, float surfH);  // -> b0

    HWND target_ = nullptr;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDXGISwapChain2> swapChain_;

    // NV12 -> RGB pipeline.
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> postCb_;  // b0: color-grade + aspect constants
    PostParams post_;              // latest params (uploaded in PresentFrame)
    int layoutMode_ = 1;           // 0 Per-Monitor, 1 Stretch, 2 Clone
    std::vector<MonitorRect> monitorRects_;  // surface-relative, when spanning

    // App-owned, shader-readable NV12 texture. Decoder output textures are not
    // reliably shader-readable, so each frame is GPU->GPU copied here (no CPU
    // transfer) and sampled through two plane SRVs.
    ComPtr<ID3D11Texture2D> nv12_;
    ComPtr<ID3D11ShaderResourceView> lumaSrv_;    // R8_UNORM
    ComPtr<ID3D11ShaderResourceView> chromaSrv_;  // R8G8_UNORM
    UINT nv12W_ = 0;
    UINT nv12H_ = 0;

    win32::UniqueHandle frameLatencyWaitable_;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

}  // namespace lwe::render
