#pragma once
// ---------------------------------------------------------------------------
// config/ConfigManager.hpp
// Thread-safe owner of the EngineConfig: INI persistence in %APPDATA%, a
// shared_mutex-guarded snapshot for cold reads, a set of lock-free atomics for
// the hot values the render thread reads every frame, and an observer list so
// GUI edits push into the live engine immediately.
//
// Concurrency model:
//   * Writers (the GUI thread) call the typed setters -> take a unique_lock,
//     mutate the struct, refresh the atomics, then notify observers OUTSIDE the
//     lock (so an observer can call back in without deadlocking).
//   * Cold readers call Snapshot() -> shared_lock, return a copy.
//   * The render thread calls the atomic getters -> no lock at all.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "config/EngineConfig.hpp"

namespace lwe::config {

enum class ConfigField {
    All,
    VideoPath,
    Monitor,
    Layout,
    Aspect,
    TargetFps,
    PauseRules,
    Volume,
    Muted,
    SmartMute,
    PlaybackSpeed,
    StartWithWindows,
    MemoryEviction,
    Color,
    Rotation,
    Schedule,
    Privacy
};

class ConfigManager {
public:
    using Observer = std::function<void(const EngineConfig&, ConfigField)>;

    ConfigManager();

    // Resolve %APPDATA%\NativeWallpaperEngine\config.ini, creating the folder
    // and writing defaults on first boot, then load it. Notifies observers(All).
    void Load();

    // Flush the current config to INI if it changed since the last save.
    bool Save();

    // Cold, consistent copy of the whole struct.
    [[nodiscard]] EngineConfig Snapshot() const;

    // --- Lock-free hot reads (render thread) ---
    [[nodiscard]] int   TargetFps() const noexcept { return aTargetFps_.load(std::memory_order_relaxed); }
    [[nodiscard]] float PlaybackSpeed() const noexcept { return aSpeed_.load(std::memory_order_relaxed); }
    [[nodiscard]] float Volume() const noexcept { return aVolume_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool  Muted() const noexcept { return aMuted_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool  MemoryEviction() const noexcept { return aMemEvict_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool  PauseOnMaximized() const noexcept { return aPauseMax_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool  PauseOnFullscreen() const noexcept { return aPauseFull_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool  PauseOnFocused() const noexcept { return aPauseFocus_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool  PauseOnBattery() const noexcept { return aPauseBatt_.load(std::memory_order_relaxed); }
    // Color / post-processing (render thread reads these lock-free each frame).
    [[nodiscard]] float Brightness() const noexcept { return aBrightness_.load(std::memory_order_relaxed); }
    [[nodiscard]] float Contrast() const noexcept { return aContrast_.load(std::memory_order_relaxed); }
    [[nodiscard]] float Saturation() const noexcept { return aSaturation_.load(std::memory_order_relaxed); }
    [[nodiscard]] float Gamma() const noexcept { return aGamma_.load(std::memory_order_relaxed); }
    [[nodiscard]] float TintR() const noexcept { return aTintR_.load(std::memory_order_relaxed); }
    [[nodiscard]] float TintG() const noexcept { return aTintG_.load(std::memory_order_relaxed); }
    [[nodiscard]] float TintB() const noexcept { return aTintB_.load(std::memory_order_relaxed); }
    [[nodiscard]] float Temperature() const noexcept { return aTemp_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool  MatchSystemTheme() const noexcept { return aMatchTheme_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool  BlackoutOnCapture() const noexcept { return aBlackout_.load(std::memory_order_relaxed); }

    // --- Typed setters (GUI thread) ---
    void SetVideoPath(std::wstring value);
    void SetMonitorIndex(int value);
    void SetLayout(LayoutMode value);
    void SetAspect(AspectMode value);
    void SetTargetFps(int value);
    void SetPauseRules(bool maximized, bool fullscreen, bool focused, bool battery);
    void SetVolume(float value);
    void SetMuted(bool value);
    void SetSmartMute(bool value);
    void SetPlaybackSpeed(float value);
    void SetStartWithWindows(bool value);
    void SetMemoryEviction(bool value);
    void SetColor(float brightness, float contrast, float saturation, float gamma,
                  float tintR, float tintG, float tintB, float temperature,
                  bool matchSystemTheme);
    void SetRotation(bool enabled, int intervalMinutes, std::wstring folder);
    void SetRotationLastSwitch(long long epochSeconds);  // engine bookkeeping
    void SetSchedule(int mode, std::wstring dayPath, std::wstring nightPath,
                     int dayStartMinutes, int nightStartMinutes, float latitude,
                     float longitude);
    void SetPrivacy(bool hideFromCapture, bool blackoutOnCapture);

    // Register a live-update observer (invoked on the thread that made the edit).
    void AddObserver(Observer observer);

    [[nodiscard]] std::wstring FilePath() const { return iniPath_; }

private:
    void ResolvePaths();
    void RefreshAtomics();  // copy hot fields -> atomics; caller holds unique_lock
    void ReadFromIni();
    void Notify(ConfigField field, const EngineConfig& snapshot);

    template <typename Mutator>
    void Mutate(ConfigField field, Mutator&& mutate);

    mutable std::shared_mutex mutex_;
    EngineConfig config_;

    std::wstring iniDir_;
    std::wstring iniPath_;
    std::atomic<bool> dirty_{false};

    // Hot atomics mirrored from config_ (float atomics are lock-free on x64).
    std::atomic<int>   aTargetFps_{60};
    std::atomic<float> aSpeed_{1.0f};
    std::atomic<float> aVolume_{1.0f};
    std::atomic<bool>  aMuted_{false};
    std::atomic<bool>  aMemEvict_{true};
    std::atomic<bool>  aPauseMax_{true};
    std::atomic<bool>  aPauseFull_{true};
    std::atomic<bool>  aPauseFocus_{false};
    std::atomic<bool>  aPauseBatt_{true};
    std::atomic<float> aBrightness_{1.0f};
    std::atomic<float> aContrast_{1.0f};
    std::atomic<float> aSaturation_{1.0f};
    std::atomic<float> aGamma_{1.0f};
    std::atomic<float> aTintR_{1.0f};
    std::atomic<float> aTintG_{1.0f};
    std::atomic<float> aTintB_{1.0f};
    std::atomic<float> aTemp_{0.0f};
    std::atomic<bool>  aMatchTheme_{false};
    std::atomic<bool>  aBlackout_{false};

    std::mutex obsMutex_;
    std::vector<Observer> observers_;
};

}  // namespace lwe::config
