#include "ui/SettingsPanel.hpp"

#include <commdlg.h>   // OPENFILENAMEW / GetOpenFileNameW (excluded by LEAN_AND_MEAN)
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <wincodec.h>  // WIC: decode the embedded logo PNG
#include <shobjidl.h>  // IFileOpenDialog (folder picker)
#include <dwmapi.h>    // DwmSetWindowAttribute (rounded corners, Win11)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include "win32/Resources.hpp"

// DWM attributes for the modern rounded window (defined on newer SDKs only).
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3  // acrylic (blur-behind)
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

// Provided by imgui_impl_win32.cpp.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace lwe::ui {

namespace {

constexpr wchar_t kPanelClassName[] = L"LWE_SettingsPanel";
constexpr int kPanelW = 1000;
constexpr int kPanelH = 650;

// --- Layout metrics (macOS System Settings) ---------------------------------
constexpr float kTitleH = 44.0f;    // unified toolbar / drag strip
constexpr float kSidebarW = 190.0f; // source-list column
constexpr float kPad = 28.0f;       // content padding
constexpr float kBtnW = 96.0f;
constexpr float kBtnH = 30.0f;

// --- Frosted-glass palette (draw-list ImU32) --------------------------------
// Fills are translucent so the Win11 acrylic backdrop (the blurred wallpaper)
// shows through. The accent is a signature violet -> the app's own identity.
constexpr ImU32 kcWin = IM_COL32(16, 17, 24, 0);          // (unused; bg is glass)
constexpr ImU32 kcSidebar = IM_COL32(14, 15, 24, 58);     // frosted source list
constexpr ImU32 kcCard = IM_COL32(146, 152, 178, 30);     // glass card / list
constexpr ImU32 kcField = IM_COL32(8, 9, 14, 140);        // sunken preview well
constexpr ImU32 kcControl = IM_COL32(255, 255, 255, 28);  // secondary button (frost)
constexpr ImU32 kcControlHi = IM_COL32(255, 255, 255, 46);// secondary hover
constexpr ImU32 kcControlDn = IM_COL32(255, 255, 255, 18);// secondary pressed
constexpr ImU32 kcSep = IM_COL32(255, 255, 255, 30);      // hairline separator
constexpr ImU32 kcGlassEdge = IM_COL32(255, 255, 255, 46);// top highlight on glass
constexpr ImU32 kcText = IM_COL32(240, 241, 245, 255);
constexpr ImU32 kcText2 = IM_COL32(176, 178, 190, 255);   // secondary label
constexpr ImU32 kcAccent = IM_COL32(129, 101, 255, 255);  // signature violet
constexpr ImU32 kcAccentHi = IM_COL32(154, 130, 255, 255);
constexpr ImU32 kcAccentDn = IM_COL32(108, 82, 226, 255);
constexpr ImU32 kcAccentGlow = IM_COL32(129, 101, 255, 90);
constexpr ImU32 kcToggleOff = IM_COL32(255, 255, 255, 40);
constexpr ImU32 kcHoverPill = IM_COL32(255, 255, 255, 22);
constexpr ImU32 kcWhite = IM_COL32(255, 255, 255, 255);

// Traffic lights.
constexpr ImU32 kcBtnHover = IM_COL32(255, 255, 255, 30);   // min hover
constexpr ImU32 kcBtnDown = IM_COL32(255, 255, 255, 18);    // min pressed
constexpr ImU32 kcCloseHover = IM_COL32(232, 17, 35, 255);  // close hover (Win red)
constexpr ImU32 kcCloseDown = IM_COL32(241, 112, 122, 255); // close pressed

// --- ImVec4 mirror for the ImGui widget style -------------------------------
constexpr ImVec4 kWinV = {0.0f, 0.0f, 0.0f, 0.0f};            // transparent (glass)
constexpr ImVec4 kCardV = {0.57f, 0.60f, 0.70f, 0.12f};       // glass card
constexpr ImVec4 kFieldV = {0.03f, 0.035f, 0.055f, 0.55f};    // sunken well
constexpr ImVec4 kControlV = {1.0f, 1.0f, 1.0f, 0.11f};       // frost control
constexpr ImVec4 kControlHiV = {1.0f, 1.0f, 1.0f, 0.18f};
constexpr ImVec4 kTextV = {0.941f, 0.945f, 0.961f, 1.0f};
constexpr ImVec4 kText2V = {0.69f, 0.70f, 0.745f, 1.0f};
constexpr ImVec4 kAccentV = {0.506f, 0.396f, 1.0f, 1.0f};     // signature violet
constexpr ImVec4 kAccentHiV = {0.604f, 0.51f, 1.0f, 1.0f};
constexpr ImVec4 kSepV = {1.0f, 1.0f, 1.0f, 0.12f};
constexpr ImVec4 kSubheadV = {0.69f, 0.70f, 0.745f, 1.0f};    // group subheaders

// Windows-style caption buttons, top-right (client coords): minimize + close.
struct CaptionButtons {
    ImVec4 minb;    // (l,t,r,b)
    ImVec4 closeb;
};

CaptionButtons CaptionButtonRects(float clientW) {
    const float bw = 46.0f, bh = kTitleH;
    CaptionButtons c;
    c.closeb = ImVec4(clientW - bw, 0.0f, clientW, bh);
    c.minb = ImVec4(clientW - 2.0f * bw, 0.0f, clientW - bw, bh);
    return c;
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

// --- Blur-behind glass (SetWindowCompositionAttribute) ----------------------
// Unlike DWMSBT acrylic (whose dark tint is fixed and heavy), this lets us pick
// the tint alpha, so the window can be genuinely see-through with a Gaussian
// blur. The API is undocumented, so it's resolved dynamically.
void EnableBlurBehind(HWND hwnd, unsigned tintAABBGGRR) {
    struct ACCENTPOLICY { int state; int flags; unsigned gradient; int anim; };
    struct WINCOMPATTR { int attrib; void* data; size_t size; };
    using SetWCA_t = BOOL(WINAPI*)(HWND, WINCOMPATTR*);
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return;
    auto setWCA = reinterpret_cast<SetWCA_t>(
        ::GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (setWCA == nullptr) return;
    // state 4 = ACCENT_ENABLE_ACRYLICBLURBEHIND (blur + our tint alpha).
    ACCENTPOLICY policy{4, 0, tintAABBGGRR, 0};
    WINCOMPATTR data{19 /*WCA_ACCENT_POLICY*/, &policy, sizeof(policy)};
    setWCA(hwnd, &data);
}

// A rounded, frosted-glass surface: translucent fill, hairline border, and a
// soft top-edge highlight so light appears to catch the glass.
void Card(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 fill,
          float rounding, bool border = true) {
    dl->AddRectFilled(a, b, fill, rounding);
    if (border) {
        // Bright hairline along the top curve = the lit glass edge.
        dl->AddLine(ImVec2(a.x + rounding, a.y + 0.5f),
                    ImVec2(b.x - rounding, a.y + 0.5f), kcGlassEdge, 1.0f);
        dl->AddRect(a, b, kcSep, rounding, 0, 1.0f);
    }
}

// The colored rounded-square icon tile + a simple white glyph, macOS-style.
void SectionIcon(ImDrawList* dl, ImVec2 c, float tile, int section) {
    static const ImU32 kTint[7] = {
        IM_COL32(10, 132, 255, 255),   // Library  — blue
        IM_COL32(94, 92, 230, 255),    // Display  — indigo
        IM_COL32(255, 159, 10, 255),   // Perf     — orange
        IM_COL32(255, 69, 58, 255),    // Audio    — red
        IM_COL32(142, 142, 147, 255),  // System   — gray
        IM_COL32(191, 90, 242, 255),   // Color    — purple
        IM_COL32(48, 209, 88, 255),    // Schedule — green
    };
    const float h = tile * 0.5f;
    const ImVec2 a(c.x - h, c.y - h), b(c.x + h, c.y + h);
    dl->AddRectFilled(a, b, kTint[section], tile * 0.28f);

    const ImU32 g = kcWhite;
    const float th = 1.6f;
    const float u = tile * 0.16f;  // glyph unit
    switch (section) {
        case 0: {  // Library — 2x2 photo grid
            const float s = u * 0.9f;
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) {
                    const ImVec2 p(c.x - u * 1.1f + i * (s + u * 0.5f),
                                   c.y - u * 1.1f + j * (s + u * 0.5f));
                    dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), g, 1.2f);
                }
            break;
        }
        case 1: {  // Display — monitor
            dl->AddRect(ImVec2(c.x - u * 1.6f, c.y - u * 1.3f),
                        ImVec2(c.x + u * 1.6f, c.y + u * 0.6f), g, 1.6f, 0, th);
            dl->AddLine(ImVec2(c.x - u * 0.8f, c.y + u * 1.4f),
                        ImVec2(c.x + u * 0.8f, c.y + u * 1.4f), g, th);
            break;
        }
        case 2: {  // Performance — ascending bars
            for (int i = 0; i < 3; ++i) {
                const float bh = u * (0.8f + i * 0.7f);
                const float x = c.x - u * 1.4f + i * u * 1.3f;
                dl->AddRectFilled(ImVec2(x, c.y + u * 1.4f - bh),
                                  ImVec2(x + u * 0.8f, c.y + u * 1.4f), g, 0.8f);
            }
            break;
        }
        case 3: {  // Audio — speaker + wave
            dl->AddRectFilled(ImVec2(c.x - u * 1.6f, c.y - u * 0.7f),
                              ImVec2(c.x - u * 0.6f, c.y + u * 0.7f), g, 0.6f);
            dl->AddTriangleFilled(ImVec2(c.x - u * 0.6f, c.y - u * 1.4f),
                                  ImVec2(c.x + u * 0.4f, c.y),
                                  ImVec2(c.x - u * 0.6f, c.y + u * 1.4f), g);
            dl->PathArcTo(ImVec2(c.x - u * 0.6f, c.y), u * 1.6f, -0.9f, 0.9f, 12);
            dl->PathStroke(g, 0, th);
            break;
        }
        case 4: {  // System — gear
            dl->AddCircle(c, u * 1.4f, g, 16, th);
            dl->AddCircleFilled(c, u * 0.5f, g);
            for (int i = 0; i < 6; ++i) {
                const float ang = i * 3.14159f / 3.0f;
                const ImVec2 d(std::cos(ang), std::sin(ang));
                dl->AddLine(ImVec2(c.x + d.x * u * 1.4f, c.y + d.y * u * 1.4f),
                            ImVec2(c.x + d.x * u * 2.0f, c.y + d.y * u * 2.0f), g, th);
            }
            break;
        }
        case 5: {  // Color — droplet
            dl->AddCircleFilled(ImVec2(c.x, c.y + u * 0.4f), u * 1.3f, g);
            dl->AddTriangleFilled(ImVec2(c.x, c.y - u * 1.8f),
                                  ImVec2(c.x - u * 1.1f, c.y + u * 0.2f),
                                  ImVec2(c.x + u * 1.1f, c.y + u * 0.2f), g);
            break;
        }
        case 6: {  // Schedule — clock
            dl->AddCircle(c, u * 1.6f, g, 18, th);
            dl->AddLine(c, ImVec2(c.x, c.y - u * 1.0f), g, th);
            dl->AddLine(c, ImVec2(c.x + u * 0.9f, c.y), g, th);
            break;
        }
        default:
            break;
    }
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
            libraryFolder_ = ui_.libraryFolder;  // restore the last-browsed folder
            ScanLibraryFolder();                 // repopulate the file list
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
    ApplyMacTheme();
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
    if (dcompVisual_) { dcompVisual_->Release(); dcompVisual_ = nullptr; }
    if (dcompTarget_) { dcompTarget_->Release(); dcompTarget_ = nullptr; }
    if (dcompDevice_) { dcompDevice_->Release(); dcompDevice_ = nullptr; }
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

    // WS_EX_NOREDIRECTIONBITMAP: no opaque GDI redirection surface, so the
    // DirectComposition swapchain's per-pixel alpha reaches the compositor and
    // the acrylic backdrop can show through.
    hwnd_ = ::CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                             MAKEINTATOM(wndClass_),
                             L"RetroWall \x2014 Settings", style, x, y, w, h,
                             nullptr, nullptr, instance_, this);
    if (hwnd_ == nullptr) return false;

    // Dark titlebar hint + rounded corners + a soft border (Windows 11).
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWORD corner = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                            sizeof(corner));
    COLORREF border = RGB(84, 84, 96);
    ::DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    // See-through blur: a light, self-chosen tint (alpha ~0x2A) over a Gaussian
    // blur of whatever is behind the window. Much more transparent than the
    // fixed dark DWMSBT acrylic. Tint is 0xAABBGGRR (a faint cool slate).
    EnableBlurBehind(hwnd_, 0x2A241E1C);
    return true;
}

bool SettingsPanel::CreateDeviceAndSwapChain() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // required for composition
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     flags, levels, ARRAYSIZE(levels),
                                     D3D11_SDK_VERSION, &device_, &obtained,
                                     &context_);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                 levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                 &device_, &obtained, &context_);
    }
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) { dxgiDevice->Release(); return false; }
    IDXGIFactory2* factory = nullptr;
    const HRESULT fhr = adapter->GetParent(IID_PPV_ARGS(&factory));
    adapter->Release();
    if (FAILED(fhr)) { dxgiDevice->Release(); return false; }

    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(rc.right - rc.left);
    sd.Height = static_cast<UINT>(rc.bottom - rc.top);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;  // per-pixel alpha for glass
    sd.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForComposition(device_, &sd, nullptr, &swapChain_);
    factory->Release();
    if (FAILED(hr)) { dxgiDevice->Release(); return false; }

    // Bind the swapchain as the window's composed content via DirectComposition.
    hr = ::DCompositionCreateDevice(dxgiDevice, IID_PPV_ARGS(&dcompDevice_));
    dxgiDevice->Release();
    if (FAILED(hr)) return false;
    if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_)))
        return false;
    if (FAILED(dcompDevice_->CreateVisual(&dcompVisual_))) return false;
    dcompVisual_->SetContent(swapChain_);
    dcompTarget_->SetRoot(dcompVisual_);
    dcompDevice_->Commit();

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
    // Segoe UI is the closest system face to SF Pro on Windows.
    bodyFont_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f);
    titleFont_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 15.0f);
    headerFont_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 23.0f);
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
    // Transparent clear: everything the UI doesn't paint (or paints with alpha)
    // reveals the acrylic backdrop. ImGui's blend over a premultiplied target
    // composites correctly, so no halos.
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    const HRESULT hr = swapChain_->Present(1, 0);
    swapChainOccluded_ = (hr == DXGI_STATUS_OCCLUDED);
}

// ---------------------------------------------------------------------------
// Theme — macOS dark appearance: rounded, flat, generous spacing.
// ---------------------------------------------------------------------------
void SettingsPanel::ApplyMacTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.GrabRounding = 10.0f;
    s.PopupRounding = 8.0f;
    s.ScrollbarRounding = 10.0f;
    s.TabRounding = 6.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 0.0f;
    s.WindowPadding = ImVec2(0, 0);
    s.ItemSpacing = ImVec2(10, 12);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.FramePadding = ImVec2(9, 6);
    s.GrabMinSize = 18.0f;
    s.ScrollbarSize = 12.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = kWinV;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.156f, 0.156f, 0.168f, 0.98f);
    c[ImGuiCol_Text] = kTextV;
    c[ImGuiCol_TextDisabled] = kText2V;
    c[ImGuiCol_FrameBg] = kCardV;
    c[ImGuiCol_FrameBgHovered] = kControlV;
    c[ImGuiCol_FrameBgActive] = kControlHiV;
    c[ImGuiCol_Header] = kAccentV;
    c[ImGuiCol_HeaderHovered] = kControlV;
    c[ImGuiCol_HeaderActive] = kAccentHiV;
    c[ImGuiCol_Button] = kControlV;
    c[ImGuiCol_ButtonHovered] = kControlHiV;
    c[ImGuiCol_ButtonActive] = ImVec4(0.212f, 0.212f, 0.231f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.925f, 0.925f, 0.933f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c[ImGuiCol_Separator] = kSepV;
    c[ImGuiCol_SeparatorHovered] = kSepV;
    c[ImGuiCol_Border] = kSepV;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.16f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.24f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.32f);
}

// ---------------------------------------------------------------------------
// Rounded, filled button. Primary = system-blue fill; secondary = graphite.
// ---------------------------------------------------------------------------
bool SettingsPanel::MacButton(const char* label, float w, float h, bool primary) {
    const ImVec2 size(w, h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    ImU32 fill;
    if (primary)
        fill = held ? kcAccentDn : (hovered ? kcAccentHi : kcAccent);
    else
        fill = held ? kcControlDn : (hovered ? kcControlHi : kcControl);
    if (primary) {  // soft violet halo under the primary action
        dl->AddRectFilled(ImVec2(p.x - 1.5f, p.y - 1.0f),
                          ImVec2(q.x + 1.5f, q.y + 3.0f), kcAccentGlow, 9.0f);
    }
    dl->AddRectFilled(p, q, fill, 7.0f);
    // Lit top edge (light catching the glass / button).
    dl->AddLine(ImVec2(p.x + 6, p.y + 0.5f), ImVec2(q.x - 6, p.y + 0.5f),
                primary ? IM_COL32(255, 255, 255, 66) : kcGlassEdge, 1.0f);
    if (!primary) dl->AddRect(p, q, kcSep, 7.0f, 0, 1.0f);

    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 tp(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f);
    dl->AddText(tp, primary ? kcWhite : kcText, label);
    return pressed;
}

// ---------------------------------------------------------------------------
// macOS pill switch — a labelled, right-aligned toggle (replaces Checkbox).
// ---------------------------------------------------------------------------
bool SettingsPanel::MacToggle(const char* label, bool* v) {
    const float rowW = 430.0f, rowH = 24.0f;
    const float trackW = 40.0f, trackH = 22.0f, knobR = 9.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(rowW, rowH));
    if (clicked) *v = !*v;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, p.y + (rowH - ts.y) * 0.5f), kcText, label);

    const ImVec2 t0(p.x + rowW - trackW, p.y + (rowH - trackH) * 0.5f);
    const ImVec2 t1(t0.x + trackW, t0.y + trackH);
    ImU32 track = *v ? kcAccent : kcToggleOff;
    if (hovered && !*v) track = kcControlHi;
    dl->AddRectFilled(t0, t1, track, trackH * 0.5f);
    const float cy = (t0.y + t1.y) * 0.5f;
    const float cx = *v ? (t1.x - knobR - 2.0f) : (t0.x + knobR + 2.0f);
    dl->AddCircleFilled(ImVec2(cx, cy), knobR, kcWhite);
    return clicked;
}

// ---------------------------------------------------------------------------
// Title strip: three traffic lights (left) + centered window title.
// ---------------------------------------------------------------------------
void SettingsPanel::DrawTitleBar(float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetMainViewport()->Pos;

    // Centered window title over the content region.
    ImFont* tf = static_cast<ImFont*>(titleFont_);
    const char* title = "RetroWall \xC2\xB7 Settings";
    const float fh = 15.0f;
    const float centerX = o.x + kSidebarW + (width - kSidebarW) * 0.5f;
    const ImVec2 ts = tf ? tf->CalcTextSizeA(fh, FLT_MAX, 0.0f, title)
                         : ImGui::CalcTextSize(title);
    const ImVec2 tp(centerX - ts.x * 0.5f, o.y + (kTitleH - fh) * 0.5f);
    if (tf) dl->AddText(tf, fh, tp, kcText2, title);
    else dl->AddText(tp, kcText2, title);

    // Windows-style caption buttons (minimize + close) at the top-right.
    const CaptionButtons cb = CaptionButtonRects(width);
    auto drawBtn = [&](const ImVec4& r, const char* id, bool isClose) -> bool {
        const ImVec2 a(o.x + r.x, o.y + r.y);
        const ImVec2 b(o.x + r.z, o.y + r.w);
        ImGui::SetCursorScreenPos(a);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(r.z - r.x, r.w - r.y));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        if (hovered) {
            const ImU32 fill = isClose ? (held ? kcCloseDown : kcCloseHover)
                                       : (held ? kcBtnDown : kcBtnHover);
            dl->AddRectFilled(a, b, fill);
        }
        const ImU32 g = (isClose && hovered) ? kcWhite : kcText;
        const float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f;
        if (isClose) {  // close: ×
            dl->AddLine(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), g, 1.2f);
            dl->AddLine(ImVec2(cx - 5, cy + 5), ImVec2(cx + 5, cy - 5), g, 1.2f);
        } else {  // minimize: −
            dl->AddLine(ImVec2(cx - 5, cy), ImVec2(cx + 5, cy), g, 1.2f);
        }
        return clicked;
    };
    if (drawBtn(cb.minb, "##min", false)) HideToTray();
    if (drawBtn(cb.closeb, "##close", true)) HideToTray();
}

// ---------------------------------------------------------------------------
// macOS source list: brand header, then a row per section with an icon tile
// and a rounded selection pill.
// ---------------------------------------------------------------------------
void SettingsPanel::DrawSidebar(float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kcSidebar);
    dl->AddLine(ImVec2(x + w, y), ImVec2(x + w, y + h), kcSep);  // divider

    // Brand header (below the traffic lights).
    const float brandY = y + kTitleH + 8.0f;
    float textX = x + 16.0f;
    if (logoSrv_ != nullptr) {
        const float ih = 22.0f;
        const float iw = ih * (logoH_ ? float(logoW_) / float(logoH_) : 1.0f);
        const ImVec2 ip(x + 16.0f, brandY);
        dl->AddImage(reinterpret_cast<ImTextureID>(logoSrv_), ip,
                     ImVec2(ip.x + iw, ip.y + ih));
        textX = ip.x + iw + 9.0f;
    }
    ImFont* bf = static_cast<ImFont*>(titleFont_);
    if (bf) dl->AddText(bf, 16.0f, ImVec2(textX, brandY + 2.0f), kcText, "RetroWall");
    else dl->AddText(ImVec2(textX, brandY + 2.0f), kcText, "RetroWall");

    static const char* kNames[7] = {"Library", "Display",  "Performance", "Audio",
                                    "System",  "Color",    "Schedule"};
    const float rowsTop = brandY + 40.0f;
    const float rowH = 34.0f;
    const float pillL = x + 8.0f, pillR = x + w - 8.0f;

    for (int i = 0; i < 7; ++i) {
        const ImVec2 rp(x, rowsTop + i * rowH);
        ImGui::SetCursorScreenPos(rp);
        char id[16];
        ::wsprintfA(id, "##side%d", i);
        if (ImGui::InvisibleButton(id, ImVec2(w, rowH))) activeTab_ = i;
        const bool sel = (activeTab_ == i);
        const bool hovered = ImGui::IsItemHovered();

        const ImVec2 a(pillL, rp.y + 2.0f), b(pillR, rp.y + rowH - 2.0f);
        if (sel) {
            // Soft violet glow + solid pill + lit top edge.
            dl->AddRectFilled(ImVec2(a.x - 2.0f, a.y - 1.5f),
                              ImVec2(b.x + 2.0f, b.y + 2.5f), kcAccentGlow, 10.0f);
            dl->AddRectFilled(a, b, kcAccent, 7.0f);
            dl->AddLine(ImVec2(a.x + 7, a.y + 0.5f), ImVec2(b.x - 7, a.y + 0.5f),
                        IM_COL32(255, 255, 255, 64), 1.0f);
        } else if (hovered) {
            dl->AddRectFilled(a, b, kcHoverPill, 7.0f);
        }

        SectionIcon(dl, ImVec2(x + 26.0f, rp.y + rowH * 0.5f), 20.0f, i);

        const ImVec2 ts = ImGui::CalcTextSize(kNames[i]);
        dl->AddText(ImVec2(x + 44.0f, rp.y + (rowH - ts.y) * 0.5f),
                    sel ? kcWhite : kcText, kNames[i]);
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
    const float W = vp->Size.x, H = vp->Size.y;

    // Base glass tint: barely-there, so the acrylic blur dominates and the panel
    // reads as clear frosted glass. Just a faint bottom-weighted darkening to
    // anchor the footer text.
    dl->AddRectFilledMultiColor(o, ImVec2(o.x + W, o.y + H),
                                IM_COL32(12, 14, 22, 8), IM_COL32(12, 14, 22, 8),
                                IM_COL32(6, 7, 12, 34), IM_COL32(6, 7, 12, 34));

    // Sidebar (full height, drawn first).
    DrawSidebar(o.x, o.y, kSidebarW, H);

    // Title strip (traffic lights + centered title).
    DrawTitleBar(W);

    // --- Content region ----------------------------------------------------
    static const char* kTitles[7] = {"Library", "Display",  "Performance", "Audio",
                                     "System",  "Color",    "Schedule"};
    const float cx = o.x + kSidebarW + kPad;
    const float cRight = o.x + W - kPad;

    // Big section title.
    ImFont* hf = static_cast<ImFont*>(headerFont_);
    const float titleY = o.y + kTitleH + 14.0f;
    if (hf) dl->AddText(hf, 23.0f, ImVec2(cx, titleY), kcText, kTitles[activeTab_]);
    else dl->AddText(ImVec2(cx, titleY), kcText, kTitles[activeTab_]);

    // Page body (transparent scroll child).
    const float bodyTop = titleY + 42.0f;
    const float bodyBot = o.y + H - 62.0f;
    ImGui::SetCursorScreenPos(ImVec2(cx, bodyTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##page", ImVec2(cRight - cx, bodyBot - bodyTop), false);
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

    // Footer: status text (left) + Close / Apply (right).
    const float footY = o.y + H - 46.0f;
    dl->AddLine(ImVec2(o.x + kSidebarW, footY - 12.0f),
                ImVec2(o.x + W, footY - 12.0f), kcSep);

    std::string status = ui_.videoPath.empty()
                             ? std::string("No wallpaper set")
                             : ("Playing  " +
                                Narrow(std::filesystem::path(ui_.videoPath)
                                           .filename().wstring()));
    if (status.size() > 64) status = status.substr(0, 61) + "...";
    dl->AddText(ImVec2(cx, footY + (kBtnH - 15.0f) * 0.5f), kcText2, status.c_str());

    const float applyX = cRight - kBtnW;
    const float closeX = applyX - 10.0f - kBtnW;
    ImGui::SetCursorScreenPos(ImVec2(closeX, footY));
    if (MacButton("Close", kBtnW, kBtnH)) HideToTray();
    ImGui::SetCursorScreenPos(ImVec2(applyX, footY));
    if (MacButton(dirty_ ? "Apply \xE2\x80\xA2" : "Apply", kBtnW, kBtnH, true))
        CommitSettings();

    ImGui::End();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Small helper: a secondary-gray group subheader.
// ---------------------------------------------------------------------------
namespace {
void Subhead(const char* text) {
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushStyleColor(ImGuiCol_Text, kSubheadV);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}
}  // namespace

// ---------------------------------------------------------------------------
// Tab pages
// ---------------------------------------------------------------------------
void SettingsPanel::TabLibrary() {
    // Folder chooser row.
    if (MacButton("Select Folder...", 150, 30)) {
        PickLibraryFolder();
    }
    ImGui::SameLine();
    if (MacButton("Single File...", 130, 30)) {
        BrowseForVideo();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (libraryFolder_.empty()) {
        ImGui::TextDisabled("No folder selected");
    } else {
        ImGui::TextDisabled("%s  \xC2\xB7  %d item%s", Narrow(libraryFolder_).c_str(),
                            static_cast<int>(libraryFiles_.size()),
                            libraryFiles_.size() == 1 ? "" : "s");
    }
    ImGui::Spacing();

    // Split: file list (left) | preview panel (right).
    const float listW = 300.0f;
    const float rowH = ImGui::GetContentRegionAvail().y;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCardV);
    ImGui::BeginChild("##filelist", ImVec2(listW, rowH), true);
    ImGui::PopStyleColor();
    if (libraryFiles_.empty()) {
        ImGui::Spacing();
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
        Subhead("Preview");

        // 16:9 preview surface (rounded well) showing the video's thumbnail.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = 320.0f, h = 180.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Card(dl, p, ImVec2(p.x + w, p.y + h), kcField, 8.0f);
        if (thumbSrv_ != nullptr && thumbW_ > 0 && thumbH_ > 0) {
            // Fit the thumbnail into the box, preserving aspect (letterbox).
            const float scale = (std::min)((w - 4) / thumbW_, (h - 4) / thumbH_);
            const float iw = thumbW_ * scale, ih = thumbH_ * scale;
            const ImVec2 a(p.x + (w - iw) * 0.5f, p.y + (h - ih) * 0.5f);
            dl->AddImage(reinterpret_cast<ImTextureID>(thumbSrv_), a,
                         ImVec2(a.x + iw, a.y + ih));
        } else if (ui_.videoPath.empty()) {
            dl->AddText(ImVec2(p.x + 14, p.y + 14), kcText2,
                        "No wallpaper selected");
        } else {
            dl->AddText(ImVec2(p.x + 14, p.y + 14), kcText2,
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
                                  pending ? kAccentHiV : kTextV);
            ImGui::TextUnformatted(pending ? "Selected  \xC2\xB7  press Apply to set"
                                           : "Active wallpaper");
            ImGui::PopStyleColor();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 340);
        ImGui::PushStyleColor(ImGuiCol_Text, kText2V);
        ImGui::TextWrapped("%s", ui_.videoPath.empty()
                                     ? "(none selected)"
                                     : Narrow(ui_.videoPath).c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

void SettingsPanel::TabDisplay() {
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

    ImGui::Spacing();
    int layout = static_cast<int>(ui_.layout);
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Layout Mode", &layout, "Per-Monitor\0Stretch\0Clone\0\0")) {
        ui_.layout = static_cast<config::LayoutMode>(layout);
        dirty_ = true;
    }
    ImGui::TextDisabled("Stretch spans all displays; Per-Monitor / Clone repeat the clip on each.");
}

void SettingsPanel::TabPerformance() {
    int fpsIdx = ui_.targetFps <= 15 ? 0 : (ui_.targetFps <= 30 ? 1 : 2);
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("Target FPS", &fpsIdx, "15\0" "30\0" "60\0\0")) {
        ui_.targetFps = config::kTargetFpsPresets[fpsIdx];
        dirty_ = true;
    }

    Subhead("Pause rules");
    bool changed = false;
    changed |= MacToggle("Pause on maximized window", &ui_.pauseOnMaximized);
    changed |= MacToggle("Pause on fullscreen app", &ui_.pauseOnFullscreen);
    changed |= MacToggle("Pause when an app is focused", &ui_.pauseOnFocused);
    changed |= MacToggle("Pause on battery / saver", &ui_.pauseOnBattery);
    if (changed) dirty_ = true;
}

void SettingsPanel::TabAudio() {
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("Master Volume", &ui_.volume, 0.0f, 1.0f, "%.2f")) {
        dirty_ = true;
    }
    if (MacToggle("Smart Mute (mute when another app plays audio)",
                  &ui_.smartMute)) {
        dirty_ = true;
    }
    if (MacToggle("Mute", &ui_.muted)) {
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
    if (MacToggle("Start with Windows", &ui_.startWithWindows)) {
        dirty_ = true;
    }
    if (MacToggle("Memory eviction while idle", &ui_.memoryEviction)) {
        dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    Subhead("Privacy");
    if (MacToggle("Black out wallpaper while a screen recorder runs",
                  &ui_.blackoutOnCapture)) {
        dirty_ = true;
    }
    ImGui::TextDisabled("Renders solid black when OBS / Bandicam / Camtasia is detected.");
    // Note: WDA_EXCLUDEFROMCAPTURE (hide-from-capture) is not offered because the
    // OS rejects it on the WorkerW wallpaper child window (not a top-level window).
}

void SettingsPanel::TabColor() {
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
    if (MacToggle("Match Windows light/dark theme", &ui_.matchSystemTheme)) {
        dirty_ = true;
    }
    ImGui::TextDisabled("Auto-nudges temperature/brightness to the system theme.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (MacButton("Neutral", 120, 30)) {   // reset the grade to identity
        ui_.brightness = ui_.contrast = ui_.gamma = 1.0f;
        ui_.saturation = 1.0f;
        ui_.tintR = ui_.tintG = ui_.tintB = 1.0f;
        ui_.temperature = 0.0f;
        dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Reset color grade to neutral. Changes apply on Apply.");
}

void SettingsPanel::TabSchedule() {
    // --- Playlist rotation -------------------------------------------------
    Subhead("Playlist Rotation");
    if (MacToggle("Auto-cycle a folder of wallpapers", &ui_.rotationEnabled)) {
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
    if (MacButton("Rotation Folder...", 170, 28)) {
        if (PickFolder(ui_.rotationFolder)) dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", ui_.rotationFolder.empty()
                                  ? "(none)"
                                  : Narrow(ui_.rotationFolder).c_str());

    ImGui::Spacing();
    ImGui::Separator();

    // --- Day / Night -------------------------------------------------------
    Subhead("Day / Night Schedule");
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("Mode", &ui_.scheduleMode,
                     "Off\0Fixed local times\0Sunrise / sunset\0")) {
        dirty_ = true;
    }

    if (ui_.scheduleMode != 0) {
        if (MacButton("Day clip...", 130, 28)) {
            if (PickVideoFile(ui_.dayVideoPath)) dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", ui_.dayVideoPath.empty()
                                      ? "(none)"
                                      : Narrow(std::filesystem::path(ui_.dayVideoPath)
                                                   .filename().wstring()).c_str());
        if (MacButton("Night clip...", 130, 28)) {
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
        // The browsed folder is a convenience, not a staged edit: persist it
        // right away so it's remembered across restarts even without Apply.
        ui_.libraryFolder = libraryFolder_;
        config_.SetLibraryFolder(libraryFolder_);
        config_.Save();
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
                // Let ImGui handle the caption buttons; drag the rest of the strip.
                const CaptionButtons cb =
                    CaptionButtonRects(static_cast<float>(rc.right));
                auto inside = [&](const ImVec4& r) {
                    return pt.x >= r.x && pt.x < r.z && pt.y >= r.y && pt.y < r.w;
                };
                if (inside(cb.minb) || inside(cb.closeb)) return HTCLIENT;
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
