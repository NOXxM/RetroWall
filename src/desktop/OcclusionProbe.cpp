#include "desktop/OcclusionProbe.hpp"

#include <dwmapi.h>

namespace lwe::desktop {

namespace {

struct ProbeContext {
    RECT monitor{};
    HWND self = nullptr;
    bool covered = false;
};

bool FullyCovers(const RECT& win, const RECT& mon) noexcept {
    return win.left <= mon.left && win.top <= mon.top &&
           win.right >= mon.right && win.bottom >= mon.bottom;
}

BOOL CALLBACK CoverProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<ProbeContext*>(lparam);

    if (hwnd == ctx->self) return TRUE;
    if (!::IsWindowVisible(hwnd)) return TRUE;
    if (::IsIconic(hwnd)) return TRUE;

    // Skip the desktop shell family itself. Our wallpaper is a CHILD of WorkerW,
    // so EnumWindows never yields our own HWND — but WorkerW/Progman ARE visible,
    // opaque, monitor-spanning top-level windows. Without this filter every probe
    // would report "occluded" and the wallpaper would never render.
    wchar_t cls[64] = {};
    if (::GetClassNameW(hwnd, cls, 64) > 0) {
        if (::lstrcmpiW(cls, L"WorkerW") == 0 ||
            ::lstrcmpiW(cls, L"Progman") == 0) {
            return TRUE;
        }
    }

    // Skip windows cloaked by DWM (other virtual desktops, suspended UWP).
    BOOL cloaked = FALSE;
    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                          sizeof(cloaked))) &&
        cloaked) {
        return TRUE;
    }

    // Skip click-through / tool layers (our own wallpaper window is one).
    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return TRUE;

    RECT wr{};
    if (!::GetWindowRect(hwnd, &wr)) return TRUE;

    if (FullyCovers(wr, ctx->monitor)) {
        ctx->covered = true;
        return FALSE;  // decisive: stop enumerating
    }
    return TRUE;
}

}  // namespace

bool IsWallpaperOccluded(HWND self) noexcept {
    HMONITOR monitor = ::MonitorFromWindow(self, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) {
        return false;  // fail open: keep rendering rather than risk a black desktop
    }

    ProbeContext ctx{};
    ctx.monitor = info.rcMonitor;
    ctx.self = self;
    ::EnumWindows(&CoverProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.covered;
}

bool IsForegroundAppActive() noexcept {
    const HWND fg = ::GetForegroundWindow();
    if (fg == nullptr) {
        return false;  // nothing focused (e.g. desktop) -> not "in an app"
    }
    // Our own windows (e.g. the settings panel) are not "another app".
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    if (pid == ::GetCurrentProcessId()) {
        return false;
    }
    wchar_t cls[64] = {};
    if (::GetClassNameW(fg, cls, 64) > 0) {
        // The desktop shell / taskbar being foreground means we are on the
        // desktop, not in an application.
        if (::lstrcmpiW(cls, L"WorkerW") == 0 ||
            ::lstrcmpiW(cls, L"Progman") == 0 ||
            ::lstrcmpiW(cls, L"Shell_TrayWnd") == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace lwe::desktop
