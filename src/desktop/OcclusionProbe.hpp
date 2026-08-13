#pragma once
// ---------------------------------------------------------------------------
// desktop/OcclusionProbe.hpp
// Occlusion detection for the composition path.
//
// Why this exists: Phase 1 detected occlusion from Present returning
// DXGI_STATUS_OCCLUDED. Flip-model *composition* swapchains generally do NOT
// return that status, so with the DirectComposition backend we detect coverage
// by inspecting the window tree instead — "is a top-level window fully covering
// the monitor our wallpaper is on?".
// ---------------------------------------------------------------------------
#include <windows.h>

namespace lwe::desktop {

// True if some visible, non-minimized, non-cloaked, opaque top-level window
// (other than our own click-through wallpaper window) completely covers the
// monitor that `self` lives on. Heuristic: it catches the common maximized /
// fullscreen case, not several tiled windows that jointly cover the screen.
[[nodiscard]] bool IsWallpaperOccluded(HWND self) noexcept;

// True if a normal application window (not the desktop shell / taskbar) is the
// current foreground window — i.e. the user is "in an app", not on the desktop.
// Used for the optional "pause when an app is focused" rule.
[[nodiscard]] bool IsForegroundAppActive() noexcept;

}  // namespace lwe::desktop
