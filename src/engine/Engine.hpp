#pragma once
// ---------------------------------------------------------------------------
// engine/Engine.hpp  (Phase 6)
// Windows + threads:
//   * controlWnd_ — hidden top-level window: power notifications + pump + quit.
//   * renderWnd_  — WorkerW child hosting the HWND video swapchain.
//   * SystemTrayManager — tray icon + menu (its own window).
//   * SettingsPanel     — ImGui/D3D11 control panel (its own thread + window),
//     renders only while visible.
//   * ConfigManager     — thread-safe settings (INI + atomics + observers).
//   * one render thread — D3D11 / DirectComposition / Media Foundation.
//
// Settings edits flow GUI -> ConfigManager -> observer -> live engine. Hot
// values (target FPS, playback speed, pause rules, eviction) are read lock-free
// from ConfigManager atomics on the render thread each frame.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "config/ConfigManager.hpp"
#include "ui/SettingsPanel.hpp"
#include "ui/SystemTrayManager.hpp"

namespace lwe::engine {

class Engine {
public:
    explicit Engine(std::wstring videoPath, bool openSettings = false);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    int Run();

private:
    // windows
    void CreateControlWindow();
    void CreateRenderWindow();
    static LRESULT CALLBACK ControlThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK RenderThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT ControlProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT RenderProc(HWND, UINT, WPARAM, LPARAM);

    // tray + settings + config
    void SetupUi();
    void OnConfigChanged(const config::EngineConfig& cfg,
                         config::ConfigField field);

    // Reposition the wallpaper surface onto the configured monitor (0 = span all
    // monitors / primary). Runs on the pump thread; resizes the swapchain.
    void ApplyMonitorTarget(int monitorIndex);

    // Rotation + day/night schedule tick (pump thread; WM_TIMER).
    void EvaluateSchedule();

    // power
    void RegisterPowerNotifications();
    void UnregisterPowerNotifications();
    void EvaluatePowerState();

    // occlusion notification
    void InstallWinEventHook();
    void RemoveWinEventHook();
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG,
                                      DWORD, DWORD);
    void OnDesktopChanged();

    // state
    void SetManualPause(bool paused);
    void RequestExit();
    void SignalWake();
    void RequestShutdown();
    void EvictWorkingSet();
    void JoinThreads();
    [[nodiscard]] std::wstring AutoStartCommandLine() const;

    // render thread
    void RenderThreadMain();

    static Engine* s_instance;

    std::wstring videoPath_;  // initial path from the command line (may be empty)
    bool         openSettings_ = false;  // --settings: open the panel on launch
    HINSTANCE    instance_ = nullptr;

    HWND controlWnd_ = nullptr;
    HWND renderWnd_ = nullptr;
    ATOM controlClass_ = 0;
    ATOM renderClass_ = 0;

    config::ConfigManager config_;
    std::unique_ptr<ui::SystemTrayManager> tray_;
    std::unique_ptr<ui::SettingsPanel> settingsPanel_;

    HWINEVENTHOOK winEventHook_ = nullptr;
    HPOWERNOTIFY  powerAcDc_ = nullptr;
    HPOWERNOTIFY  powerSaver_ = nullptr;

    // Pause coordination (guarded by stateMutex_).
    std::mutex              stateMutex_;
    std::condition_variable stateCv_;
    bool running_ = true;
    bool wake_ = true;
    bool manualPause_ = false;   // transient tray pause (not persisted)
    bool powerFreeze_ = false;

    // Live video swap (guarded by stateMutex_).
    bool         videoReload_ = false;
    std::wstring pendingVideoPath_;

    std::atomic<std::uint64_t> pendingResize_{0};
    std::thread renderThread_;

    std::string renderError_;
};

}  // namespace lwe::engine
