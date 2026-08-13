#pragma once
// ---------------------------------------------------------------------------
// desktop/DesktopAttach.hpp
// The "WorkerW split" technique: coerce the shell into forking a WorkerW that
// renders *behind* the desktop icons, then reparent our render window into it.
// ---------------------------------------------------------------------------
#include <windows.h>

namespace lwe::desktop {

// Where we should parent the wallpaper window.
struct DesktopHost {
    HWND worker = nullptr;         // target parent (a WorkerW, or Progman fallback)
    bool spawnedWorkerW = false;   // true: dedicated WorkerW found; false: using Progman
};

// Send the undocumented 0x052C message to Progman to spawn the WorkerW pair,
// then enumerate top-level windows to find the WorkerW that sits behind the
// SHELLDLL_DefView icon layer. Throws lwe::win32::Win32Error on hard failures.
[[nodiscard]] DesktopHost AcquireDesktopHost();

// Reparent `child` into the host (SetParent) and stretch it to fill the layer.
void AttachToDesktop(HWND child, const DesktopHost& host);

}  // namespace lwe::desktop
