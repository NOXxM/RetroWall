#pragma once
// ---------------------------------------------------------------------------
// config/EngineConfig.hpp
// The single source of truth for all user-configurable engine settings. Plain
// data — no behavior — so it can be copied cheaply for thread-safe snapshots
// (see ConfigManager) and serialized to INI.
// ---------------------------------------------------------------------------
#include <string>

namespace lwe::config {

enum class LayoutMode {
    PerMonitor = 0,  // each monitor renders the clip independently
    Stretch = 1,     // one surface stretched across the whole virtual desktop
    Clone = 2        // same frame mirrored to every monitor
};

enum class AspectMode {
    Fill = 0,     // cover the target, cropping overflow
    Fit = 1,      // letterbox/pillarbox to preserve aspect
    Stretch = 2   // distort to exactly fill
};

struct EngineConfig {
    // --- Library ---
    std::wstring videoPath;
    std::wstring libraryFolder;  // last folder browsed in the Library tab (sticky)

    // --- Display & multi-monitor ---
    int        monitorIndex = 0;  // 0 == primary / all (per LayoutMode)
    LayoutMode layout = LayoutMode::PerMonitor;
    AspectMode aspect = AspectMode::Fill;

    // --- Performance rules ---
    int   targetFps = 60;          // render cap: 15 / 30 / 60
    bool  pauseOnMaximized = true;
    bool  pauseOnFullscreen = true;
    bool  pauseOnFocused = false;  // pause whenever any app window has focus
    bool  pauseOnBattery = true;   // battery / battery-saver

    // --- Audio & playback ---
    float volume = 1.0f;           // 0.0 .. 1.0
    bool  smartMute = true;        // auto-mute when another app plays audio
    float playbackSpeed = 1.0f;    // 0.25 .. 2.0
    bool  muted = false;

    // --- System ---
    bool startWithWindows = false;
    bool memoryEviction = true;    // trim working set while frozen

    // --- Color / post-processing (applied live in the video shader) ---
    float brightness = 1.0f;   // 0.5 .. 1.5   (multiplicative)
    float contrast   = 1.0f;   // 0.5 .. 1.5   (around mid-gray)
    float saturation = 1.0f;   // 0.0 .. 2.0
    float gamma      = 1.0f;   // 0.5 .. 2.5
    float tintR = 1.0f;        // per-channel tint multipliers
    float tintG = 1.0f;
    float tintB = 1.0f;
    float temperature = 0.0f;  // -1 warm .. +1 cool
    bool  matchSystemTheme = false;  // auto-nudge temperature to Win light/dark

    // --- Rotation (auto-cycle a folder of clips) ---
    bool         rotationEnabled = false;
    int          rotationIntervalMinutes = 30;  // switch every N minutes
    std::wstring rotationFolder;                // folder to cycle
    long long    rotationLastSwitch = 0;        // epoch seconds; engine-managed

    // --- Day / Night schedule ---
    int          scheduleMode = 0;  // 0 = off, 1 = fixed times, 2 = astronomical
    std::wstring dayVideoPath;
    std::wstring nightVideoPath;
    int          dayStartMinutes = 7 * 60;    // fixed mode: minutes since midnight
    int          nightStartMinutes = 19 * 60;
    float        latitude = 0.0f;             // astronomical mode
    float        longitude = 0.0f;

    // --- Privacy ---
    bool hideFromCapture = false;    // exclude the wallpaper from screen captures
    bool blackoutOnCapture = false;  // local blackout when a capture app is detected
};

// Allowed target-FPS presets for the GUI combo.
inline constexpr int kTargetFpsPresets[] = {15, 30, 60};

}  // namespace lwe::config
