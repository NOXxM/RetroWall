#include "ui/SystemTrayManager.hpp"

#include <utility>

namespace lwe::ui {

namespace {

// Internal messages, posted so cross-thread updates run on the UI thread.
constexpr UINT WM_TRAY_REFRESH = WM_APP + 0x02;
constexpr UINT WM_TRAY_BALLOON = WM_APP + 0x03;

// Context-menu command ids.
constexpr UINT kCmdPlayPause = 0xA001;
constexpr UINT kCmdMute = 0xA002;
constexpr UINT kCmdSettings = 0xA003;
constexpr UINT kCmdExit = 0xA004;

constexpr wchar_t kTrayClassName[] = L"LWE_TrayWindow";
constexpr UINT kTrayIconId = 1;

}  // namespace

SystemTrayManager::SystemTrayManager(HINSTANCE instance, std::wstring tooltip,
                                     HICON icon, Callbacks callbacks)
    : instance_(instance),
      baseTooltip_(std::move(tooltip)),
      icon_(icon != nullptr ? icon : ::LoadIconW(nullptr, IDI_APPLICATION)),
      callbacks_(std::move(callbacks)) {
    taskbarCreatedMsg_ = ::RegisterWindowMessageW(L"TaskbarCreated");
    CreateHelperWindow();
    AddIcon();
}

SystemTrayManager::~SystemTrayManager() {
    RemoveIcon();
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (wndClass_ != 0) {
        ::UnregisterClassW(MAKEINTATOM(wndClass_), instance_);
        wndClass_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Helper window
// ---------------------------------------------------------------------------
void SystemTrayManager::CreateHelperWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SystemTrayManager::WndProcThunk;
    wc.hInstance = instance_;
    wc.lpszClassName = kTrayClassName;
    wndClass_ = ::RegisterClassExW(&wc);
    if (wndClass_ == 0) {
        return;  // best-effort: without a window there is no tray icon
    }

    // Hidden 1x1 top-level window (NOT message-only) so SetForegroundWindow
    // works for correct context-menu dismissal.
    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, MAKEINTATOM(wndClass_),
                              L"LWE Tray", WS_POPUP, 0, 0, 1, 1, nullptr,
                              nullptr, instance_, this);
}

LRESULT CALLBACK SystemTrayManager::WndProcThunk(HWND hwnd, UINT msg,
                                                 WPARAM wparam, LPARAM lparam) {
    SystemTrayManager* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SystemTrayManager*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SystemTrayManager*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->WndProc(hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT SystemTrayManager::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                   LPARAM lparam) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
        // Explorer restarted: our icon is gone from the tray — re-add it.
        iconAdded_ = false;
        AddIcon();
        return 0;
    }

    switch (msg) {
        case WM_TRAY_ICON_MSG: {
            // Legacy (non-V4) model: lParam is the mouse message.
            switch (LOWORD(lparam)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowContextMenu();
                    break;
                case WM_LBUTTONDBLCLK:
                    if (callbacks_.onOpenSettings) {
                        callbacks_.onOpenSettings();
                    }
                    break;
                default:
                    break;
            }
            return 0;
        }
        case WM_TRAY_REFRESH:
            RefreshIcon();
            return 0;
        case WM_TRAY_BALLOON:
            EmitPendingBalloon();
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

// ---------------------------------------------------------------------------
// Shell_NotifyIcon plumbing (UI thread only)
// ---------------------------------------------------------------------------
NOTIFYICONDATAW SystemTrayManager::BaseIconData() const noexcept {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    return nid;
}

void SystemTrayManager::AddIcon() {
    if (hwnd_ == nullptr || iconAdded_) {
        return;
    }
    NOTIFYICONDATAW nid = BaseIconData();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_ICON_MSG;
    nid.hIcon = icon_;
    const std::wstring tip = ComposeTooltip();
    ::lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
    iconAdded_ = (::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
    if (!iconAdded_) {
        ::OutputDebugStringW(L"[LWE] Shell_NotifyIcon(NIM_ADD) failed\n");
    }
}

void SystemTrayManager::RemoveIcon() {
    if (!iconAdded_) {
        return;
    }
    NOTIFYICONDATAW nid = BaseIconData();
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    iconAdded_ = false;
}

void SystemTrayManager::RefreshIcon() {
    if (!iconAdded_) {
        return;
    }
    NOTIFYICONDATAW nid = BaseIconData();
    nid.uFlags = NIF_TIP;
    const std::wstring tip = ComposeTooltip();
    ::lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void SystemTrayManager::EmitPendingBalloon() {
    if (!iconAdded_) {
        return;
    }
    std::wstring title;
    std::wstring text;
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        title.swap(pendingBalloonTitle_);
        text.swap(pendingBalloonText_);
    }
    if (text.empty()) {
        return;
    }
    NOTIFYICONDATAW nid = BaseIconData();
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    ::lstrcpynW(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle));
    ::lstrcpynW(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo));
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

std::wstring SystemTrayManager::ComposeTooltip() const {
    bool paused = false;
    bool muted = false;
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        paused = paused_;
        muted = muted_;
    }
    std::wstring tip = baseTooltip_;
    if (paused || muted) {
        tip += L" \x2014 ";  // em dash
        tip += paused ? L"Paused" : L"Playing";
        if (muted) {
            tip += L", Muted";
        }
    }
    return tip;
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------
void SystemTrayManager::ShowContextMenu() {
    bool paused = false;
    bool muted = false;
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        paused = paused_;
        muted = muted_;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    ::AppendMenuW(menu, MF_STRING, kCmdPlayPause, paused ? L"Play" : L"Pause");
    ::AppendMenuW(menu, MF_STRING | (muted ? MF_CHECKED : MF_UNCHECKED), kCmdMute,
                  L"Mute Audio");
    ::AppendMenuW(menu, MF_STRING, kCmdSettings, L"Open Settings...");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit Engine");

    POINT pt{};
    ::GetCursorPos(&pt);
    // Required so the menu closes when the user clicks elsewhere.
    ::SetForegroundWindow(hwnd_);
    const int cmd = ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x,
                                     pt.y, 0, hwnd_, nullptr);
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
    ::DestroyMenu(menu);

    switch (static_cast<UINT>(cmd)) {
        case kCmdPlayPause:
            SetPaused(!IsPaused());  // immediate visual toggle
            if (callbacks_.onPlayPause) callbacks_.onPlayPause();
            break;
        case kCmdMute:
            SetMuted(!IsMuted());    // dynamic checkmark from the tray
            if (callbacks_.onMuteToggle) callbacks_.onMuteToggle();
            break;
        case kCmdSettings:
            if (callbacks_.onOpenSettings) callbacks_.onOpenSettings();
            break;
        case kCmdExit:
            if (callbacks_.onExit) callbacks_.onExit();
            break;
        default:
            break;  // 0 == dismissed
    }
}

// ---------------------------------------------------------------------------
// Thread-safe state API
// ---------------------------------------------------------------------------
void SystemTrayManager::SetPaused(bool paused) {
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        if (paused_ == paused) {
            return;
        }
        paused_ = paused;
    }
    // hwnd_ is fixed after construction; posting marshals the UI update to the
    // owning thread even when called from the render thread.
    if (hwnd_ != nullptr) {
        ::PostMessageW(hwnd_, WM_TRAY_REFRESH, 0, 0);
    }
}

void SystemTrayManager::SetMuted(bool muted) {
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        if (muted_ == muted) {
            return;
        }
        muted_ = muted;
    }
    if (hwnd_ != nullptr) {
        ::PostMessageW(hwnd_, WM_TRAY_REFRESH, 0, 0);
    }
}

bool SystemTrayManager::IsPaused() const noexcept {
    std::scoped_lock<std::mutex> lock(stateMutex_);
    return paused_;
}

bool SystemTrayManager::IsMuted() const noexcept {
    std::scoped_lock<std::mutex> lock(stateMutex_);
    return muted_;
}

void SystemTrayManager::ShowBalloon(std::wstring title, std::wstring text) {
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        pendingBalloonTitle_ = std::move(title);
        pendingBalloonText_ = std::move(text);
    }
    if (hwnd_ != nullptr) {
        ::PostMessageW(hwnd_, WM_TRAY_BALLOON, 0, 0);
    }
}

}  // namespace lwe::ui
