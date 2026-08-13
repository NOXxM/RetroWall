#pragma once
// ---------------------------------------------------------------------------
// ui/SystemTrayManager.hpp
// A self-contained Win32 system-tray handler wrapping Shell_NotifyIconW.
//
// It owns a hidden helper window that receives the tray callback message
// (WM_TRAY_ICON_MSG — note: the common "WM_TRAM_ICON_MSG" spelling is a typo for
// TRAY) and hosts the right-click context menu:
//     Play / Pause | Mute Audio (checkable) | Open Settings... | --- | Exit Engine
// Left double-click raises the Settings window.
//
// Threading contract:
//   * Construct and destroy on the UI (message-pump) thread. All Shell_NotifyIcon
//     calls happen there.
//   * SetPaused / SetMuted / ShowBalloon are SAFE TO CALL FROM ANY THREAD: they
//     update mutex-guarded state and marshal the actual tray update to the UI
//     thread via a posted message. The context menu reads that state under the
//     lock when it is built, so checkmarks/labels are always current.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <shellapi.h>  // NOTIFYICONDATAW, Shell_NotifyIconW

#include <functional>
#include <mutex>
#include <string>

namespace lwe::ui {

// The tray icon's notification callback message.
inline constexpr UINT WM_TRAY_ICON_MSG = WM_APP + 0x01;

class SystemTrayManager {
public:
    // User-action hooks. Any may be empty. Invoked on the UI thread.
    struct Callbacks {
        std::function<void()> onPlayPause;    // "Play / Pause" chosen
        std::function<void()> onMuteToggle;   // "Mute Audio" chosen
        std::function<void()> onOpenSettings; // "Open Settings..." or double-click
        std::function<void()> onExit;         // "Exit Engine" chosen
    };

    // `icon` may be nullptr (a shared stock icon is used). The manager does not
    // take ownership of a caller-supplied icon.
    SystemTrayManager(HINSTANCE instance, std::wstring tooltip, HICON icon,
                      Callbacks callbacks);
    ~SystemTrayManager();

    SystemTrayManager(const SystemTrayManager&) = delete;
    SystemTrayManager& operator=(const SystemTrayManager&) = delete;

    // Thread-safe state updates (reflected in the menu label/checkmark + tooltip).
    void SetPaused(bool paused);
    void SetMuted(bool muted);

    [[nodiscard]] bool IsPaused() const noexcept;
    [[nodiscard]] bool IsMuted() const noexcept;

    // Thread-safe balloon/toast (e.g. "Minimized to tray").
    void ShowBalloon(std::wstring title, std::wstring text);

    [[nodiscard]] HWND MessageWindow() const noexcept { return hwnd_; }

private:
    void CreateHelperWindow();
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    [[nodiscard]] NOTIFYICONDATAW BaseIconData() const noexcept;
    void AddIcon();
    void RemoveIcon();
    void RefreshIcon();        // NIM_MODIFY tooltip/icon on the UI thread
    void EmitPendingBalloon(); // NIM_MODIFY with NIF_INFO on the UI thread
    void ShowContextMenu();
    [[nodiscard]] std::wstring ComposeTooltip() const;

    HINSTANCE    instance_ = nullptr;
    std::wstring baseTooltip_;
    HICON        icon_ = nullptr;
    Callbacks    callbacks_;

    ATOM wndClass_ = 0;
    HWND hwnd_ = nullptr;
    bool iconAdded_ = false;

    // "TaskbarCreated" — broadcast when Explorer (re)starts, so we re-add.
    UINT taskbarCreatedMsg_ = 0;

    mutable std::mutex stateMutex_;
    bool paused_ = false;
    bool muted_ = false;
    std::wstring pendingBalloonTitle_;
    std::wstring pendingBalloonText_;
};

}  // namespace lwe::ui
