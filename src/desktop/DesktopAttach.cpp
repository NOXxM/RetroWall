#include "desktop/DesktopAttach.hpp"

#include "win32/Error.hpp"
#include "win32/Log.hpp"

namespace lwe::desktop {

using win32::ThrowIfFalse;
using win32::ThrowIfNull;
using win32::Win32Error;

namespace {

// Undocumented Progman message that forces Explorer to create the WorkerW
// wallpaper surface behind the desktop icons.
constexpr UINT kSpawnWorkerW = 0x052C;

struct EnumContext {
    HWND siblingWorker = nullptr;  // WorkerW that follows the DefView host (Win10)
    HWND emptyWorker = nullptr;    // visible WorkerW WITHOUT icons (wallpaper layer)
};

// Enumerate top-level windows: log the desktop family and collect candidates.
BOOL CALLBACK ScanProc(HWND top, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumContext*>(lparam);

    wchar_t cls[64] = {};
    ::GetClassNameW(top, cls, 64);
    const bool isWorker = (::lstrcmpiW(cls, L"WorkerW") == 0);
    const bool isProgman = (::lstrcmpiW(cls, L"Progman") == 0);
    if (!isWorker && !isProgman) {
        return TRUE;
    }

    const HWND defView =
        ::FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    const BOOL visible = ::IsWindowVisible(top);
    RECT r{};
    ::GetWindowRect(top, &r);
    log::Writef(L"  scan %p cls=%s defView=%p vis=%d rect=%d,%d,%d,%d",
                reinterpret_cast<void*>(top), cls,
                reinterpret_cast<void*>(defView), visible ? 1 : 0, r.left, r.top,
                r.right, r.bottom);

    if (defView != nullptr) {
        // The WorkerW immediately after the icon host is the classic Win10
        // wallpaper layer.
        const HWND sib = ::FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        if (sib != nullptr) {
            ctx->siblingWorker = sib;
        }
    } else if (isWorker && visible) {
        // A visible WorkerW that does NOT host the icons is a wallpaper-layer
        // candidate regardless of Z-order (more robust than the sibling trick).
        ctx->emptyWorker = top;
    }
    return TRUE;
}

}  // namespace

DesktopHost AcquireDesktopHost() {
    HWND progman =
        ThrowIfNull(::FindWindowW(L"Progman", nullptr), "FindWindow(Progman)");

    // Poke Progman to fork the wallpaper WorkerW. Send both documented parameter
    // variants — different Windows builds respond to different ones.
    DWORD_PTR reply = 0;
    ::SendMessageTimeoutW(progman, kSpawnWorkerW, 0, 0, SMTO_NORMAL, 1000, &reply);
    ::SendMessageTimeoutW(progman, kSpawnWorkerW, 0x0000000D, 0x00000001,
                          SMTO_NORMAL, 1000, &reply);

    log::Write(L"AcquireDesktopHost: scanning desktop window family...");
    EnumContext ctx{};
    ::EnumWindows(&ScanProc, reinterpret_cast<LPARAM>(&ctx));

    // Some Windows 11 builds host the wallpaper WorkerW as a CHILD of Progman.
    const HWND workerChildOfProgman =
        ::FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    const HWND defViewOfProgman =
        ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    log::Writef(L"  progman=%p defViewChild=%p workerChild=%p sibWorker=%p emptyWorker=%p",
                reinterpret_cast<void*>(progman),
                reinterpret_cast<void*>(defViewOfProgman),
                reinterpret_cast<void*>(workerChildOfProgman),
                reinterpret_cast<void*>(ctx.siblingWorker),
                reinterpret_cast<void*>(ctx.emptyWorker));

    DesktopHost host{};
    if (ctx.siblingWorker != nullptr) {
        host.worker = ctx.siblingWorker;
        host.spawnedWorkerW = true;
    } else if (ctx.emptyWorker != nullptr) {
        host.worker = ctx.emptyWorker;
        host.spawnedWorkerW = true;
    } else if (workerChildOfProgman != nullptr) {
        host.worker = workerChildOfProgman;
        host.spawnedWorkerW = true;
    } else {
        host.worker = progman;  // last-resort fallback
        host.spawnedWorkerW = false;
    }
    log::Writef(L"AcquireDesktopHost chose worker=%p spawnedWorkerW=%d",
                reinterpret_cast<void*>(host.worker), host.spawnedWorkerW ? 1 : 0);
    return host;
}

void AttachToDesktop(HWND child, const DesktopHost& host) {
    ThrowIfNull(host.worker, "AttachToDesktop: null host window");

    // SetParent returns the previous parent, or null on failure. Because null
    // is also a legal "previous parent", disambiguate via GetLastError.
    ::SetLastError(ERROR_SUCCESS);
    const HWND previous = ::SetParent(child, host.worker);
    if (previous == nullptr && ::GetLastError() != ERROR_SUCCESS) {
        throw Win32Error(static_cast<long>(::GetLastError()), "SetParent");
    }

    // Fill the host layer. Prefer the WorkerW's own client rect; fall back to
    // the full virtual screen if the parent reports an empty rect.
    RECT rc{};
    if (::GetClientRect(host.worker, &rc) && (rc.right - rc.left) > 0 &&
        (rc.bottom - rc.top) > 0) {
        ThrowIfFalse(::SetWindowPos(child, HWND_BOTTOM, 0, 0,
                                    rc.right - rc.left, rc.bottom - rc.top,
                                    SWP_NOACTIVATE | SWP_SHOWWINDOW),
                     "SetWindowPos(worker client rect)");
    } else {
        const int width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        ThrowIfFalse(::SetWindowPos(child, HWND_BOTTOM, 0, 0, width, height,
                                    SWP_NOACTIVATE | SWP_SHOWWINDOW),
                     "SetWindowPos(virtual screen)");
    }

    RECT after{};
    ::GetClientRect(child, &after);
    log::Writef(L"AttachToDesktop: child=%p newParent=%p size=%dx%d",
                reinterpret_cast<void*>(child),
                reinterpret_cast<void*>(::GetParent(child)),
                static_cast<int>(after.right), static_cast<int>(after.bottom));
}

}  // namespace lwe::desktop
