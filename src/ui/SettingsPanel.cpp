#include "ui/SettingsPanel.hpp"

#include <commdlg.h>   // OPENFILENAMEW / GetOpenFileNameW (excluded by LEAN_AND_MEAN)
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <wincodec.h>  // WIC: decode the embedded logo PNG
#include <shobjidl.h>  // IFileOpenDialog (folder picker)

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include "win32/Resources.hpp"

// Provided by imgui_impl_win32.cpp.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace lwe::ui {

namespace {

constexpr wchar_t kPanelClassName[] = L"LWE_SettingsPanel";
constexpr int kPanelW = 1000;
constexpr int kPanelH = 650;

// Layout metrics (classic "Find" dialog).
constexpr float kTitleH = 30.0f;
constexpr float kMenuH = 22.0f;
constexpr float kTabH = 24.0f;
constexpr float kStatusH = 22.0f;
constexpr float kMargin = 12.0f;
constexpr float kBtnColW = 100.0f;

// --- Windows 95/98 chiseled palette, DARK mode (draw-list ImU32) ------------
constexpr ImU32 kFace = IM_COL32(58, 58, 60, 255);      // dialog face
constexpr ImU32 kFaceLite = IM_COL32(72, 72, 75, 255);  // raised page
constexpr ImU32 kFaceHi = IM_COL32(84, 84, 88, 255);    // button hover
constexpr ImU32 kFaceDn = IM_COL32(44, 44, 46, 255);    // button pressed
constexpr ImU32 kHilite = IM_COL32(124, 124, 128, 255); // outer light bevel
constexpr ImU32 kLight = IM_COL32(94, 94, 98, 255);     // inner light bevel
constexpr ImU32 kShadow = IM_COL32(30, 30, 32, 255);    // inner dark bevel
constexpr ImU32 kDkShadow = IM_COL32(0, 0, 0, 255);     // outer dark bevel
constexpr ImU32 kFieldBg = IM_COL32(24, 24, 26, 255);   // sunken input
constexpr ImU32 kTextCol = IM_COL32(230, 230, 232, 255);
constexpr ImU32 kTextDimCol = IM_COL32(132, 132, 134, 255);
constexpr ImU32 kAccent = IM_COL32(46, 96, 160, 255);   // selection / menu hi
constexpr ImU32 kTabUnsel = IM_COL32(50, 50, 52, 255);
constexpr ImU32 kTitleTop = IM_COL32(48, 50, 60, 255);  // dark title gradient
constexpr ImU32 kTitleBot = IM_COL32(18, 18, 24, 255);
constexpr ImU32 kTitleGloss = IM_COL32(90, 94, 110, 255);

// --- ImVec4 mirror for the ImGui widget style -------------------------------
constexpr ImVec4 kFaceV = {0.227f, 0.227f, 0.235f, 1.0f};
constexpr ImVec4 kFieldV = {0.094f, 0.094f, 0.102f, 1.0f};
constexpr ImVec4 kTextV = {0.902f, 0.902f, 0.910f, 1.0f};
constexpr ImVec4 kTextDimV = {0.517f, 0.517f, 0.525f, 1.0f};
constexpr ImVec4 kAccentV = {0.180f, 0.376f, 0.627f, 1.0f};
constexpr ImVec4 kAccentHiV = {0.243f, 0.455f, 0.729f, 1.0f};
constexpr ImVec4 kShadowV = {0.118f, 0.118f, 0.125f, 1.0f};
constexpr ImVec4 kHeaderBlue = {0.53f, 0.72f, 1.0f, 1.0f};  // section titles

// Classic 2px chisel: raised = light top-left / dark bottom-right (sunken swaps).
void Bevel(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool raised) {
    const ImU32 oLT = raised ? kHilite : kDkShadow;
    const ImU32 oRB = raised ? kDkShadow : kHilite;
    const ImU32 iLT = raised ? kLight : kShadow;
    const ImU32 iRB = raised ? kShadow : kLight;
    const float r = b.x - 1, bot = b.y - 1;
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(r, a.y), oLT);       // outer top
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, bot), oLT);     // outer left
    dl->AddLine(ImVec2(a.x, bot), ImVec2(r, bot), oRB);       // outer bottom
    dl->AddLine(ImVec2(r, a.y), ImVec2(r, bot), oRB);         // outer right
    dl->AddLine(ImVec2(a.x + 1, a.y + 1), ImVec2(r - 1, a.y + 1), iLT);
    dl->AddLine(ImVec2(a.x + 1, a.y + 1), ImVec2(a.x + 1, bot - 1), iLT);
    dl->AddLine(ImVec2(a.x + 1, bot - 1), ImVec2(r - 1, bot - 1), iRB);
    dl->AddLine(ImVec2(r - 1, a.y + 1), ImVec2(r - 1, bot - 1), iRB);
}

struct CapButtons {
    ImVec4 minb;    // (l,t,r,b) in client coords
    ImVec4 closeb;
};

CapButtons TitleButtonRects(float clientW) {
    const float bw = 22.0f, bh = 20.0f, top = 5.0f, gap = 2.0f, margin = 6.0f;
    CapButtons b;
    b.closeb = ImVec4(clientW - margin - bw, top, clientW - margin, top + bh);
    b.minb = ImVec4(b.closeb.x - gap - bw, top, b.closeb.x - gap, top + bh);
    return b;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                        static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

SettingsPanel::SettingsPanel(HINSTANCE instance, config::ConfigManager& config)
    : instance_(instance), config_(config) {
    wakeEvent_ = ::CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE, nullptr);
    thread_ = std::thread(&SettingsPanel::ThreadMain, this);
}

SettingsPanel::~SettingsPanel() {
    running_.store(false);
    if (wakeEvent_ != nullptr) {
        ::SetEvent(wakeEvent_);  // wake the thread if it's parked
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (wakeEvent_ != nullptr) {
        ::CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Thread main: block while hidden, render while visible.
// ---------------------------------------------------------------------------
void SettingsPanel::ThreadMain() {
    if (!InitGraphics()) {
        running_.store(false);
        ShutdownGraphics();
        return;
    }

    while (running_.load()) {
        if (!visible_.load()) {
            ::WaitForSingleObject(wakeEvent_, INFINITE);
            if (!running_.load()) break;
        }

        if (showRequested_.exchange(false)) {
            ::ShowWindow(hwnd_, SW_SHOW);
            if (::IsIconic(hwnd_)) {
                ::ShowWindow(hwnd_, SW_RESTORE);
            }
            ::SetForegroundWindow(hwnd_);
            ui_ = config_.Snapshot();
            dirty_ = false;
            LoadThumbnail(ui_.videoPath);  // preview the currently-active clip
            visible_.store(true);
        }

        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (!running_.load()) break;
        if (!visible_.load()) continue;

        RenderFrame();
    }

    ShutdownGraphics();
}

void SettingsPanel::Show() {
    showRequested_.store(true);
    if (wakeEvent_ != nullptr) {
        ::SetEvent(wakeEvent_);
    }
}

void SettingsPanel::HideToTray() {
    visible_.store(false);
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    config_.Save();
}

// ---------------------------------------------------------------------------
// Graphics init/teardown (panel thread)
// ---------------------------------------------------------------------------
bool SettingsPanel::InitGraphics() {
    comInit_ = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    if (!CreatePanelWindow()) return false;
    if (!CreateDeviceAndSwapChain()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    LoadFonts();
    ApplyClassicTheme();
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_, context_);
    LoadLogoTexture();
    return true;
}

void SettingsPanel::ShutdownGraphics() {
    if (thumbSrv_) { thumbSrv_->Release(); thumbSrv_ = nullptr; }
    if (logoSrv_) { logoSrv_->Release(); logoSrv_ = nullptr; }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    CleanupRenderTarget();
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (wndClass_ != 0) {
        ::UnregisterClassW(MAKEINTATOM(wndClass_), instance_);
        wndClass_ = 0;
    }
    if (comInit_) { ::CoUninitialize(); comInit_ = false; }
}

bool SettingsPanel::CreatePanelWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SettingsPanel::WndProcThunk;
    wc.hInstance = instance_;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPanelClassName;
    wc.hIcon = static_cast<HICON>(::LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APPICON),
                                              IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APPICON),
                                               IMAGE_ICON, 16, 16, 0));
    wndClass_ = ::RegisterClassExW(&wc);
    if (wndClass_ == 0) return false;

    RECT rc{0, 0, kPanelW, kPanelH};
    const DWORD style = WS_POPUP;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = (::GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int y = (::GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    hwnd_ = ::CreateWindowExW(WS_EX_APPWINDOW, MAKEINTATOM(wndClass_),
                             L"RetroWall \x2014 Settings", style, x, y, w, h,
                             nullptr, nullptr, instance_, this);
    return hwnd_ != nullptr;
}

bool SettingsPanel::CreateDeviceAndSwapChain() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
        D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &obtained, &context_);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = ::D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
            D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &obtained, &context_);
    }
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void SettingsPanel::CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) &&
        backBuffer != nullptr) {
        device_->CreateRenderTargetView(backBuffer, nullptr, &rtv_);
        backBuffer->Release();
    }
}

void SettingsPanel::CleanupRenderTarget() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

// ---------------------------------------------------------------------------
// Fonts + logo
// ---------------------------------------------------------------------------
void SettingsPanel::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    bodyFont_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 15.0f);
    titleFont_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahomabd.ttf", 15.0f);
    if (bodyFont_ == nullptr) {
        io.Fonts->AddFontDefault();
    }
    io.Fonts->Build();
}

bool SettingsPanel::LoadLogoTexture() {
    HRSRC res = ::FindResourceW(instance_, MAKEINTRESOURCEW(IDR_LOGO_PNG), RT_RCDATA);
    if (res == nullptr) return false;
    HGLOBAL handle = ::LoadResource(instance_, res);
    if (handle == nullptr) return false;
    void* data = ::LockResource(handle);
    const DWORD size = ::SizeofResource(instance_, res);
    if (data == nullptr || size == 0) return false;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    do {
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(data), size))) break;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr,
                                                    WICDecodeMetadataCacheOnLoad,
                                                    &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(factory->CreateFormatConverter(&conv))) break;
        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom))) break;
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4,
                                    static_cast<UINT>(px.size()), px.data()))) break;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem = px.data(); srd.SysMemPitch = w * 4;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device_->CreateTexture2D(&td, &srd, &tex))) break;
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = td.Format;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        const HRESULT hr = device_->CreateShaderResourceView(tex, &svd, &logoSrv_);
        tex->Release();
        if (FAILED(hr)) break;
        logoW_ = static_cast<int>(w);
        logoH_ = static_cast<int>(h);
        ok = true;
    } while (false);

    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void SettingsPanel::RenderFrame() {
    if (swapChainOccluded_ &&
        swapChain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
        ::Sleep(10);
        return;
    }

    if (resizeW_ != 0 && resizeH_ != 0) {
        CleanupRenderTarget();
        swapChain_->ResizeBuffers(0, resizeW_, resizeH_, DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
        resizeW_ = resizeH_ = 0;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    BuildUi();

    ImGui::Render();
    const float clear[4] = {0.227f, 0.227f, 0.235f, 1.0f};  // dialog face
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    const HRESULT hr = swapChain_->Present(1, 0);
    swapChainOccluded_ = (hr == DXGI_STATUS_OCCLUDED);
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
void SettingsPanel::ApplyClassicTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.GrabRounding = 0.0f;
    s.PopupRounding = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.WindowPadding = ImVec2(0, 0);
    s.ItemSpacing = ImVec2(10, 9);
    s.FramePadding = ImVec2(6, 4);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = kFaceV;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.267f, 0.267f, 0.275f, 1.0f);
    c[ImGuiCol_Text] = kTextV;
    c[ImGuiCol_TextDisabled] = kTextDimV;
    c[ImGuiCol_FrameBg] = kFieldV;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.14f, 0.16f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.16f, 0.18f, 1.0f);
    c[ImGuiCol_Header] = kAccentV;
    c[ImGuiCol_HeaderHovered] = kAccentHiV;
    c[ImGuiCol_HeaderActive] = kAccentV;
    c[ImGuiCol_Button] = kFaceV;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.33f, 0.33f, 0.34f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.20f, 0.21f, 1.0f);
    c[ImGuiCol_SliderGrab] = kAccentV;
    c[ImGuiCol_SliderGrabActive] = kAccentHiV;
    c[ImGuiCol_CheckMark] = kTextV;
    c[ImGuiCol_Separator] = kShadowV;
    c[ImGuiCol_Border] = kShadowV;
    c[ImGuiCol_PopupBg] = ImVec4(0.267f, 0.267f, 0.275f, 1.0f);
}

// ---------------------------------------------------------------------------
// Beveled 3D button (raised; inverts to sunken when pressed).
// ---------------------------------------------------------------------------
bool SettingsPanel::XpButton(const char* label, float w, float h) {
    const ImVec2 size(w, h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, held ? kFaceDn : (hovered ? kFaceHi : kFace));
    Bevel(dl, p, q, !held);

    const ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 tp(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f);
    if (held) { tp.x += 1; tp.y += 1; }
    dl->AddText(tp, kTextCol, label);
    return pressed;
}

// ---------------------------------------------------------------------------
// Title bar: dark gradient, RW logo, title, minimize + close.
// ---------------------------------------------------------------------------
void SettingsPanel::DrawTitleBar(float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetMainViewport()->Pos;
    const ImVec2 tl(o.x, o.y);
    const ImVec2 br(o.x + width, o.y + kTitleH);
    dl->AddRectFilledMultiColor(tl, br, kTitleTop, kTitleTop, kTitleBot, kTitleBot);
    dl->AddLine(ImVec2(tl.x, o.y + 1), ImVec2(br.x, o.y + 1), kTitleGloss);

    float textX = o.x + 9.0f;
    if (logoSrv_ != nullptr) {
        const float ih = 20.0f;
        const float iw = ih * (logoH_ ? float(logoW_) / float(logoH_) : 1.0f);
        const ImVec2 ip(o.x + 8.0f, o.y + (kTitleH - ih) * 0.5f);
        dl->AddImage(reinterpret_cast<ImTextureID>(logoSrv_), ip,
                     ImVec2(ip.x + iw, ip.y + ih));
        textX = ip.x + iw + 8.0f;
    }

    ImFont* tf = static_cast<ImFont*>(titleFont_);
    const char* title = "RetroWall  \xC2\xB7  Find Settings";
    const float fh = 15.0f;
    const ImVec2 tp(textX, o.y + (kTitleH - fh) * 0.5f);
    if (tf) {
        dl->AddText(tf, fh, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 180), title);
        dl->AddText(tf, fh, tp, IM_COL32(240, 240, 245, 255), title);
    } else {
        dl->AddText(tp, IM_COL32(240, 240, 245, 255), title);
    }

    // caption buttons (beveled gray squares, classic style) ----------------
    const CapButtons cb = TitleButtonRects(width);
    auto drawCap = [&](const ImVec4& r, const char* id, bool isClose) -> bool {
        const ImVec2 bp(o.x + r.x, o.y + r.y);
        const ImVec2 bq(o.x + r.z, o.y + r.w);
        ImGui::SetCursorScreenPos(bp);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(r.z - r.x, r.w - r.y));
        const bool held = ImGui::IsItemActive();
        dl->AddRectFilled(bp, bq, held ? kFaceDn : kFace);
        Bevel(dl, bp, bq, !held);
        const float cx = (bp.x + bq.x) * 0.5f + (held ? 1 : 0);
        const float cy = (bp.y + bq.y) * 0.5f + (held ? 1 : 0);
        if (isClose) {
            dl->AddLine(ImVec2(cx - 3, cy - 3), ImVec2(cx + 4, cy + 4), kTextCol, 1.4f);
            dl->AddLine(ImVec2(cx - 3, cy + 3), ImVec2(cx + 4, cy - 4), kTextCol, 1.4f);
        } else {
            dl->AddLine(ImVec2(cx - 3, cy + 3), ImVec2(cx + 3, cy + 3), kTextCol, 1.6f);
        }
        return clicked;
    };
    if (drawCap(cb.minb, "##min", false)) HideToTray();
    if (drawCap(cb.closeb, "##close", true)) HideToTray();
}

// ---------------------------------------------------------------------------
// Classic notched tab row. The selected tab pops up and merges with the page.
// ---------------------------------------------------------------------------
void SettingsPanel::DrawTabRow(float x, float y, float /*w*/) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* tabs[] = {"Library", "Display", "Performance", "Audio",
                          "System", "Color", "Schedule"};
    float tx = x;
    for (int i = 0; i < 7; ++i) {
        const bool sel = (activeTab_ == i);
        const ImVec2 ts = ImGui::CalcTextSize(tabs[i]);
        const float tw = ts.x + 24.0f;
        const float top = sel ? y - 2.0f : y + 1.0f;
        const ImVec2 a(tx, top);
        const ImVec2 b(tx + tw, y + kTabH + (sel ? 3.0f : 0.0f));

        ImGui::SetCursorScreenPos(ImVec2(tx, top));
        if (ImGui::InvisibleButton(tabs[i], ImVec2(tw, (y + kTabH) - top))) {
            activeTab_ = i;
        }
        dl->AddRectFilled(a, b, sel ? kFaceLite : kTabUnsel);
        // top + side bevels (no bottom, so it reads as connected to the page)
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x - 1, a.y), kHilite);      // top
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, b.y), kHilite);          // left
        dl->AddLine(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + 1), kLight);
        dl->AddLine(ImVec2(b.x - 1, a.y), ImVec2(b.x - 1, b.y), kDkShadow);  // right
        dl->AddLine(ImVec2(b.x - 2, a.y + 1), ImVec2(b.x - 2, b.y), kShadow);

        const ImVec2 tp(tx + (tw - ts.x) * 0.5f, y + (kTabH - ts.y) * 0.5f + (sel ? -1 : 0));
        dl->AddText(tp, sel ? kTextCol : kTextDimCol, tabs[i]);
        tx += tw + 2.0f;
    }
}

// ---------------------------------------------------------------------------
// UI layout
// ---------------------------------------------------------------------------
void SettingsPanel::BuildUi() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 o = vp->Pos;
    ImGui::SetNextWindowPos(o);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    DrawTitleBar(vp->Size.x);

    // geometry --------------------------------------------------------------
    const float top = kTitleH + kMargin;
    const float btnX = o.x + vp->Size.x - kMargin - kBtnColW;
    const float pageX = o.x + kMargin;
    const float pageR = btnX - kMargin;
    const float tabsY = o.y + top;
    const float pageY = tabsY + kTabH;
    const float pageB = o.y + vp->Size.y - kStatusH - kMargin;

    // raised tab-page panel
    dl->AddRectFilled(ImVec2(pageX, pageY), ImVec2(pageR, pageB), kFaceLite);
    Bevel(dl, ImVec2(pageX, pageY), ImVec2(pageR, pageB), true);

    // tabs (drawn after the page so the selected tab covers the page's top edge)
    DrawTabRow(pageX, tabsY, pageR - pageX);

    // page content (transparent ImGui child inside the raised panel)
    ImGui::SetCursorScreenPos(ImVec2(pageX + 12, pageY + 12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##page", ImVec2(pageR - pageX - 24, pageB - pageY - 24), false);
    switch (activeTab_) {
        case 0: TabLibrary(); break;
        case 1: TabDisplay(); break;
        case 2: TabPerformance(); break;
        case 3: TabAudio(); break;
        case 4: TabSystem(); break;
        case 5: TabColor(); break;
        case 6: TabSchedule(); break;
        default: break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // right button column (Apply commits every tab's edits; browsing lives in
    // the Library tab now) -------------------------------------------------
    float by = tabsY;
    ImGui::SetCursorScreenPos(ImVec2(btnX, by));
    if (XpButton(dirty_ ? "Apply *" : "Apply", kBtnColW, 26)) CommitSettings();
    by += 32;
    ImGui::SetCursorScreenPos(ImVec2(btnX, by));
    if (XpButton("Close", kBtnColW, 26)) HideToTray();

    // the RW logo as the dialog's "magnifier" mascot
    if (logoSrv_ != nullptr) {
        const float ls = 56.0f;
        const float lx = btnX + (kBtnColW - ls) * 0.5f;
        const float ly = pageB - ls - 6.0f;
        dl->AddImage(reinterpret_cast<ImTextureID>(logoSrv_), ImVec2(lx, ly),
                     ImVec2(lx + ls, ly + ls));
    }

    // status bar (sunken) ---------------------------------------------------
    {
        const ImVec2 sp(o.x, o.y + vp->Size.y - kStatusH);
        const ImVec2 sq(o.x + vp->Size.x, o.y + vp->Size.y);
        dl->AddRectFilled(sp, sq, kFace);
        dl->AddLine(sp, ImVec2(sq.x, sp.y), kShadow);
        dl->AddLine(ImVec2(sp.x, sp.y + 1), ImVec2(sq.x, sp.y + 1), kHilite);
        std::string status = ui_.videoPath.empty()
                                 ? std::string("Ready")
                                 : ("Playing:  " + Narrow(ui_.videoPath));
        if (status.size() > 96) status = status.substr(0, 93) + "...";
        dl->AddText(ImVec2(sp.x + 10, sp.y + 4), kTextCol, status.c_str());
    }

    // window frame
    dl->AddRect(o, ImVec2(o.x + vp->Size.x, o.y + vp->Size.y), kDkShadow);

    ImGui::End();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Tab pages
// ---------------------------------------------------------------------------
void SettingsPanel::TabLibrary() {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderBlue);
    ImGui::TextUnformatted("Library");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Folder chooser row ----------------------------------------------------
    if (XpButton("Select Folder...", 150, 26)) {
        PickLibraryFolder();
    }
    ImGui::SameLine();
    if (XpButton("Single File...", 130, 26)) {
        BrowseForVideo();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (libraryFolder_.empty()) {
        ImGui::TextDisabled("(no folder selected)");
    } else {
        ImGui::TextDisabled("%s  \xC2\xB7  %d item%s", Narrow(libraryFolder_).c_str(),
                            static_cast<int>(libraryFiles_.size()),
                            libraryFiles_.size() == 1 ? "" : "s");
    }
    ImGui::Spacing();

    // Split: file list (left) | preview panel (right) -----------------------
    const float listW = 300.0f;
    const float rowH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##filelist", ImVec2(listW, rowH), true);
    if (libraryFiles_.empty()) {
        ImGui::TextDisabled("Pick a folder to scan for");
        ImGui::TextDisabled("video wallpapers.");
    } else {
        for (int i = 0; i < static_cast<int>(libraryFiles_.size()); ++i) {
            const std::filesystem::path path(libraryFiles_[i]);
            const std::string name = Narrow(path.filename().wstring());
            if (ImGui::Selectable(name.c_str(), librarySelected_ == i)) {
                SelectLibraryItem(i);  // instant preview = set as active wallpaper
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##preview", ImVec2(0, rowH), false);
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kHeaderBlue);
        ImGui::TextUnformatted("Preview");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // 16:9 preview surface (sunken) showing the video's thumbnail.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = 320.0f, h = 180.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kFieldBg);
        Bevel(dl, p, ImVec2(p.x + w, p.y + h), false);
        if (thumbSrv_ != nullptr && thumbW_ > 0 && thumbH_ > 0) {
            // Fit the thumbnail into the box, preserving aspect (letterbox).
            const float scale = (std::min)((w - 4) / thumbW_, (h - 4) / thumbH_);
            const float iw = thumbW_ * scale, ih = thumbH_ * scale;
            const ImVec2 a(p.x + (w - iw) * 0.5f, p.y + (h - ih) * 0.5f);
            dl->AddImage(reinterpret_cast<ImTextureID>(thumbSrv_), a,
                         ImVec2(a.x + iw, a.y + ih));
        } else if (ui_.videoPath.empty()) {
            dl->AddText(ImVec2(p.x + 12, p.y + 12), kTextDimCol,
                        "No wallpaper selected");
        } else {
            dl->AddText(ImVec2(p.x + 12, p.y + 12), kTextDimCol,
                        "No thumbnail available");
        }
        ImGui::Dummy(ImVec2(w, h));

        ImGui::Spacing();
        // Reflect whether the selection is applied or still pending.
        const bool pending = (ui_.videoPath != config_.Snapshot().videoPath);
        if (ui_.videoPath.empty()) {
            ImGui::TextUnformatted("Selected file:  (none)");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  pending ? kHeaderBlue : kTextV);
            ImGui::TextUnformatted(pending ? "Selected  \xC2\xB7  press Apply to set"
                                           : "Active wallpaper");
            ImGui::PopStyleColor();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 340);
        ImGui::TextWrapped("%s", ui_.videoPath.empty()
                                     ? "(none selected)"
                                     : Narrow(ui_.videoPath).c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

void SettingsPanel::TabDisplay() {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderBlue);
    ImGui::TextUnformatted("Display & Multi-Monitor");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    const int monitors = ::GetSystemMetrics(SM_CMONITORS);
    const std::string preview =
        ui_.monitorIndex == 0 ? "All / Primary"
                              : ("Monitor " + std::to_string(ui_.monitorIndex));
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Monitor", preview.c_str())) {
        for (int i = 0; i <= monitors; ++i) {
            const std::string label =
                i == 0 ? "All / Primary" : ("Monitor " + std::to_string(i));
            if (ImGui::Selectable(label.c_str(), ui_.monitorIndex == i)) {
                ui_.monitorIndex = i;   // staged; committed by Apply
                dirty_ = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(press Apply)");

    int aspect = static_cast<int>(ui_.aspect);
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Aspect Ratio", &aspect, "Fill\0Fit\0Stretch\0\0")) {
        ui_.aspect = static_cast<config::AspectMode>(aspect);
        dirty_ = true;
    }
    ImGui::TextDisabled("Fill = cover/crop  \xC2\xB7  Fit = letterbox  \xC2\xB7  Stretch = distort");

    // Layout Mode is a multi-monitor rendering feature that isn't wired to the
    // render path yet — shown disabled so it doesn't look broken.
    ImGui::Spacing();
    int layout = static_cast<int>(ui_.layout);
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("Layout Mode", &layout, "Per-Monitor\0Stretch\0Clone\0\0");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Multi-monitor layout — coming soon.");
}

void SettingsPanel::TabPerformance() {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderBlue);
    ImGui::TextUnformatted("Performance Rules");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    int fpsIdx = ui_.targetFps <= 15 ? 0 : (ui_.targetFps <= 30 ? 1 : 2);
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("Target FPS", &fpsIdx, "15\0" "30\0" "60\0\0")) {
        ui_.targetFps = config::kTargetFpsPresets[fpsIdx];
        dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Pause rules");
    bool changed = false;
    changed |= ImGui::Checkbox("Pause on maximized window", &ui_.pauseOnMaximized);
    changed |= ImGui::Checkbox("Pause on fullscreen app", &ui_.pauseOnFullscreen);
    changed |= ImGui::Checkbox("Pause when an app is focused", &ui_.pauseOnFocused);
    changed |= ImGui::Checkbox("Pause on battery / saver", &ui_.pauseOnBattery);
    if (changed) dirty_ = true;
}

void SettingsPanel::TabAudio() {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderBlue);
    ImGui::TextUnformatted("Audio & Playback");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("Master Volume", &ui_.volume, 0.0f, 1.0f, "%.2f")) {
        dirty_ = true;
    }
    if (ImGui::Checkbox("Smart Mute (mute when another app plays audio)",
                        &ui_.smartMute)) {
        dirty_ = true;
    }
    if (ImGui::Checkbox("Mute", &ui_.muted)) {
        dirty_ = true;
    }
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("Playback Speed", &ui_.playbackSpeed, 0.25f, 2.0f, "%.2fx")) {
        dirty_ = true;
    }
    ImGui::TextDisabled(
        "Audio is a future pipeline addition; these persist and drive the hook.");
}

void SettingsPanel::TabSystem() {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderBlue);
    ImGui::TextUnformatted("System");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Checkbox("Start with Windows", &ui_.startWithWindows)) {
        dirty_ = true;
    }
    if (ImGui::Checkbox("Memory eviction while idle", &ui_.memoryEviction)) {
        dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderBlue);
    ImGui::TextUnformatted("Privacy");
    ImGui::PopStyleColor();
    if (ImGui::Checkbox("Black out wallpaper while a screen recorder runs",
                        &ui_.blackoutOnCapture)) {
        dirty_ = true;
    }
    ImGui::TextDisabled("Renders solid black when OBS / Bandicam / Camtasia is detected.");
    // Note: WDA_EXCLUDEFROMCAPTURE (hide-from-capture) is not offered because the
    // OS rejects it on the WorkerW wallpaper child window (not a top-level window).
}

void SettingsPanel::TabColor() {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderBlue);
    ImGui::TextUnformatted("Color & Post-Processing");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("Brightness", &ui_.brightness, 0.5f, 1.5f, "%.2f")) dirty_ = true;
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("Contrast", &ui_.contrast, 0.5f, 1.5f, "%.2f")) dirty_ = true;
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("Saturation", &ui_.saturation, 0.0f, 2.0f, "%.2f")) dirty_ = true;
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("Gamma", &ui_.gamma, 0.5f, 2.5f, "%.2f")) dirty_ = true;
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("Temperature", &ui_.temperature, -1.0f, 1.0f, "%.2f")) dirty_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(warm .. cool)");

    ImGui::Spacing();
    float tint[3] = {ui_.tintR, ui_.tintG, ui_.tintB};
    if (ImGui::ColorEdit3("Color Tint", tint,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha)) {
        ui_.tintR = tint[0]; ui_.tintG = tint[1]; ui_.tintB = tint[2];
        dirty_ = true;
    }

    ImGui::Spacing();
    if (ImGui::Checkbox("Match Windows light/dark theme", &ui_.matchSystemTheme)) {
        dirty_ = true;
    }
    ImGui::TextDisabled("Auto-nudges temperature/brightness to the system theme.");

    ImGui::Spacing();
    ImGui::Separator();
    if (XpButton("Neutral", 120, 26)) {   // reset the grade to identity (not a 'defaults' reset)
        ui_.brightness = ui_.contrast = ui_.gamma = 1.0f;
        ui_.saturation = 1.0f;
        ui_.tintR = ui_.tintG = ui_.tintB = 1.0f;
        ui_.temperature = 0.0f;
        dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Reset color grade to neutral. Changes apply on Apply.");
}

void SettingsPanel::TabSchedule() {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderBlue);
    ImGui::TextUnformatted("Scheduling & Rotation");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Playlist rotation -------------------------------------------------
    ImGui::TextUnformatted("Playlist Rotation");
    if (ImGui::Checkbox("Auto-cycle a folder of wallpapers", &ui_.rotationEnabled)) {
        dirty_ = true;
    }

    static const int kIntervals[] = {5, 15, 30, 60, 120, 1440};
    static const char* kIntervalLabels =
        "Every 5 min\0Every 15 min\0Every 30 min\0Hourly\0Every 2 hours\0Daily\0";
    int ivIdx = 2;
    for (int i = 0; i < 6; ++i) {
        if (ui_.rotationIntervalMinutes == kIntervals[i]) { ivIdx = i; break; }
    }
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("Interval", &ivIdx, kIntervalLabels)) {
        ui_.rotationIntervalMinutes = kIntervals[ivIdx];
        dirty_ = true;
    }
    if (XpButton("Rotation Folder...", 170, 24)) {
        if (PickFolder(ui_.rotationFolder)) dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", ui_.rotationFolder.empty()
                                  ? "(none)"
                                  : Narrow(ui_.rotationFolder).c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Day / Night -------------------------------------------------------
    ImGui::TextUnformatted("Day / Night Schedule");
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("Mode", &ui_.scheduleMode,
                     "Off\0Fixed local times\0Sunrise / sunset\0")) {
        dirty_ = true;
    }

    if (ui_.scheduleMode != 0) {
        if (XpButton("Day clip...", 130, 24)) {
            if (PickVideoFile(ui_.dayVideoPath)) dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", ui_.dayVideoPath.empty()
                                      ? "(none)"
                                      : Narrow(std::filesystem::path(ui_.dayVideoPath)
                                                   .filename().wstring()).c_str());
        if (XpButton("Night clip...", 130, 24)) {
            if (PickVideoFile(ui_.nightVideoPath)) dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", ui_.nightVideoPath.empty()
                                      ? "(none)"
                                      : Narrow(std::filesystem::path(ui_.nightVideoPath)
                                                   .filename().wstring()).c_str());
    }

    if (ui_.scheduleMode == 1) {  // fixed times
        int dayH = ui_.dayStartMinutes / 60;
        int nightH = ui_.nightStartMinutes / 60;
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderInt("Day starts (hour)", &dayH, 0, 23, "%02d:00")) {
            ui_.dayStartMinutes = dayH * 60;
            dirty_ = true;
        }
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderInt("Night starts (hour)", &nightH, 0, 23, "%02d:00")) {
            ui_.nightStartMinutes = nightH * 60;
            dirty_ = true;
        }
    } else if (ui_.scheduleMode == 2) {  // astronomical
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputFloat("Latitude", &ui_.latitude, 0.0f, 0.0f, "%.4f")) dirty_ = true;
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputFloat("Longitude", &ui_.longitude, 0.0f, 0.0f, "%.4f")) dirty_ = true;
        ImGui::TextDisabled("Sun times use your system date/timezone. North & East positive.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Rotation and Day/Night both drive the active wallpaper;");
    ImGui::TextDisabled("Day/Night takes precedence when its mode is not Off.");
}

void SettingsPanel::BrowseForVideo() {
    std::wstring file;
    if (PickVideoFile(file)) {
        librarySelected_ = -1;
        StageVideo(file);  // preview only; applied when Apply is hit
    }
}

// ---------------------------------------------------------------------------
// Apply: push the whole working copy into ConfigManager (which fans each field
// out to the live engine via observers), then persist to INI.
// ---------------------------------------------------------------------------
void SettingsPanel::CommitSettings() {
    // Video source: only push if it actually changed, so re-applying unrelated
    // settings doesn't needlessly restart the current clip.
    if (ui_.videoPath != config_.Snapshot().videoPath) {
        config_.SetVideoPath(ui_.videoPath);     // -> engine swaps the wallpaper
    }
    config_.SetMonitorIndex(ui_.monitorIndex);   // -> engine repositions surface
    config_.SetLayout(ui_.layout);
    config_.SetAspect(ui_.aspect);
    config_.SetTargetFps(ui_.targetFps);
    config_.SetPauseRules(ui_.pauseOnMaximized, ui_.pauseOnFullscreen,
                          ui_.pauseOnFocused, ui_.pauseOnBattery);
    config_.SetVolume(ui_.volume);
    config_.SetSmartMute(ui_.smartMute);
    config_.SetMuted(ui_.muted);
    config_.SetPlaybackSpeed(ui_.playbackSpeed);
    config_.SetStartWithWindows(ui_.startWithWindows);
    config_.SetMemoryEviction(ui_.memoryEviction);
    config_.SetColor(ui_.brightness, ui_.contrast, ui_.saturation, ui_.gamma,
                     ui_.tintR, ui_.tintG, ui_.tintB, ui_.temperature,
                     ui_.matchSystemTheme);
    config_.SetRotation(ui_.rotationEnabled, ui_.rotationIntervalMinutes,
                        ui_.rotationFolder);
    config_.SetSchedule(ui_.scheduleMode, ui_.dayVideoPath, ui_.nightVideoPath,
                        ui_.dayStartMinutes, ui_.nightStartMinutes, ui_.latitude,
                        ui_.longitude);
    config_.SetPrivacy(ui_.hideFromCapture, ui_.blackoutOnCapture);
    config_.Save();
    dirty_ = false;
}

// ---------------------------------------------------------------------------
// Folder / file choosers
// ---------------------------------------------------------------------------
bool SettingsPanel::PickFolder(std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        return false;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    bool picked = false;
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) &&
                path != nullptr) {
                out = path;
                ::CoTaskMemFree(path);
                picked = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return picked;
}

bool SettingsPanel::PickVideoFile(std::wstring& out) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter =
        L"Video Files\0"
        L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.asf;*.flv;*.ts;*.m2ts;"
        L"*.mts;*.mpg;*.mpeg;*.3gp;*.ogv\0"
        L"All Files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetOpenFileNameW(&ofn)) {
        out = file;
        return true;
    }
    return false;
}

void SettingsPanel::PickLibraryFolder() {
    if (PickFolder(libraryFolder_)) {
        ScanLibraryFolder();
    }
}

void SettingsPanel::ScanLibraryFolder() {
    libraryFiles_.clear();
    librarySelected_ = -1;
    if (libraryFolder_.empty()) return;

    // Media Foundation decodes video only; these are the containers we accept.
    static const wchar_t* kExts[] = {
        L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi", L".wmv", L".asf",
        L".flv", L".ts",  L".m2ts", L".mts", L".mpg", L".mpeg", L".3gp", L".ogv"};

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        fs::path(libraryFolder_), fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; it != end && libraryFiles_.size() < 1000; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::wstring ext = it->path().extension().wstring();
        for (const wchar_t* e : kExts) {
            if (::_wcsicmp(ext.c_str(), e) == 0) {
                libraryFiles_.push_back(it->path().wstring());
                break;
            }
        }
    }
    std::sort(libraryFiles_.begin(), libraryFiles_.end());

    // Highlight the currently-active clip if it lives in this folder.
    for (int i = 0; i < static_cast<int>(libraryFiles_.size()); ++i) {
        if (libraryFiles_[i] == ui_.videoPath) { librarySelected_ = i; break; }
    }
}

void SettingsPanel::SelectLibraryItem(int index) {
    if (index < 0 || index >= static_cast<int>(libraryFiles_.size())) return;
    librarySelected_ = index;
    StageVideo(libraryFiles_[index]);  // preview only; applied when Apply is hit
}

// Stage a video as the pending selection: update the preview + mark dirty. The
// desktop wallpaper is NOT touched until CommitSettings() (the Apply button).
void SettingsPanel::StageVideo(const std::wstring& path) {
    ui_.videoPath = path;
    dirty_ = true;
    LoadThumbnail(path);
}

// Ask the shell for the file's thumbnail (the same frame Explorer shows) and
// upload it to a D3D texture for the preview panel. Best-effort.
bool SettingsPanel::LoadThumbnail(const std::wstring& path) {
    if (thumbSrv_) { thumbSrv_->Release(); thumbSrv_ = nullptr; }
    thumbW_ = thumbH_ = 0;
    if (path.empty() || device_ == nullptr) return false;

    IShellItem* item = nullptr;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                             IID_PPV_ARGS(&item)))) {
        return false;
    }
    IShellItemImageFactory* factory = nullptr;
    const HRESULT qi = item->QueryInterface(IID_PPV_ARGS(&factory));
    item->Release();
    if (FAILED(qi)) return false;

    HBITMAP hbmp = nullptr;
    const SIZE want{320, 180};
    const HRESULT hr = factory->GetImage(
        want, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hbmp);
    factory->Release();
    if (FAILED(hr) || hbmp == nullptr) return false;

    BITMAP bm{};
    ::GetObject(hbmp, sizeof(bm), &bm);
    const int w = bm.bmWidth, h = bm.bmHeight;
    bool ok = false;
    if (w > 0 && h > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
        HDC dc = ::GetDC(nullptr);
        const int scan = ::GetDIBits(dc, hbmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
        ::ReleaseDC(nullptr, dc);
        if (scan == h) {
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // force opaque

            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // GetDIBits gives BGRA
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA srd{};
            srd.pSysMem = px.data(); srd.SysMemPitch = w * 4;
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(device_->CreateTexture2D(&td, &srd, &tex))) {
                D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
                svd.Format = td.Format;
                svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                svd.Texture2D.MipLevels = 1;
                if (SUCCEEDED(device_->CreateShaderResourceView(tex, &svd, &thumbSrv_))) {
                    thumbW_ = w; thumbH_ = h; ok = true;
                }
                tex->Release();
            }
        }
    }
    ::DeleteObject(hbmp);
    return ok;
}

// ---------------------------------------------------------------------------
// Window proc
// ---------------------------------------------------------------------------
LRESULT CALLBACK SettingsPanel::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam,
                                             LPARAM lparam) {
    SettingsPanel* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsPanel*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsPanel*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->WndProc(hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT SettingsPanel::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return 1;
    }

    switch (msg) {
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ScreenToClient(hwnd, &pt);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            if (pt.y >= 0 && pt.y < static_cast<int>(kTitleH)) {
                const CapButtons cb = TitleButtonRects(static_cast<float>(rc.right));
                auto inside = [&](const ImVec4& r) {
                    return pt.x >= r.x && pt.x < r.z && pt.y >= r.y && pt.y < r.w;
                };
                if (inside(cb.minb) || inside(cb.closeb)) {
                    return HTCLIENT;
                }
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && device_ != nullptr) {
                resizeW_ = LOWORD(lparam);
                resizeH_ = HIWORD(lparam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_MINIMIZE) {
                HideToTray();
                return 0;
            }
            if ((wparam & 0xFFF0) == SC_KEYMENU) {
                return 0;
            }
            break;
        case WM_CLOSE:
            HideToTray();
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace lwe::ui
