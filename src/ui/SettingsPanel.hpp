#pragma once
// ---------------------------------------------------------------------------
// ui/SettingsPanel.hpp
// A dark-themed Settings control panel built with Dear ImGui on a lightweight
// Direct3D 11 renderer (separate from the wallpaper's device).
//
// It runs on its OWN thread that owns the window + device + ImGui context:
//   * hidden  -> the thread blocks on an event: 0% CPU / 0% GPU.
//   * visible -> the thread renders with vsync (no busy loop) and pumps its
//                window's messages.
// Closing or minimizing the window HIDES it (back to the tray) rather than
// destroying it. All edits are written straight into the shared ConfigManager,
// which fans them out to the live engine.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <d3d11.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "config/ConfigManager.hpp"

namespace lwe::ui {

class SettingsPanel {
public:
    SettingsPanel(HINSTANCE instance, config::ConfigManager& config);
    ~SettingsPanel();

    SettingsPanel(const SettingsPanel&) = delete;
    SettingsPanel& operator=(const SettingsPanel&) = delete;

    // Thread-safe: request the panel be shown and focused.
    void Show();

private:
    void ThreadMain();
    bool InitGraphics();
    void ShutdownGraphics();
    bool CreatePanelWindow();
    bool CreateDeviceAndSwapChain();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void RenderFrame();
    void HideToTray();

    // UI
    void BuildUi();
    void ApplyMacTheme();       // macOS dark appearance (rounded, flat)
    void LoadFonts();
    bool LoadLogoTexture();     // WIC-decode the embedded RW logo -> D3D texture
    void DrawTitleBar(float width);          // traffic lights + centered title
    void DrawSidebar(float x, float y, float w, float h);  // macOS source list
    bool MacButton(const char* label, float w, float h, bool primary = false);
    bool MacToggle(const char* label, bool* v);  // pill switch (checkbox replacement)
    void TabLibrary();
    void TabDisplay();
    void TabPerformance();
    void TabAudio();
    void TabSystem();
    void TabColor();
    void TabSchedule();
    void BrowseForVideo();
    bool PickFolder(std::wstring& out);     // generic folder chooser
    bool PickVideoFile(std::wstring& out);  // generic single-video chooser

    void CommitSettings();      // push the working copy into ConfigManager (Apply)
    void PickLibraryFolder();   // folder chooser -> ScanLibraryFolder
    void ScanLibraryFolder();   // enumerate video files under libraryFolder_
    void SelectLibraryItem(int index);       // stage a selection (applied on Apply)
    void StageVideo(const std::wstring& path);  // set pending video + load preview
    bool LoadThumbnail(const std::wstring& path);  // Shell thumbnail -> D3D texture

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    HINSTANCE instance_ = nullptr;
    config::ConfigManager& config_;

    std::thread       thread_;
    std::atomic<bool> running_{true};
    std::atomic<bool> visible_{false};
    std::atomic<bool> showRequested_{false};
    HANDLE            wakeEvent_ = nullptr;

    // Graphics — touched only on the panel thread.
    ATOM                    wndClass_ = 0;
    HWND                    hwnd_ = nullptr;
    ID3D11Device*           device_ = nullptr;
    ID3D11DeviceContext*    context_ = nullptr;
    IDXGISwapChain*         swapChain_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    UINT                    resizeW_ = 0;
    UINT                    resizeH_ = 0;
    bool                    swapChainOccluded_ = false;

    int                  activeTab_ = 0;
    bool                 dirty_ = false;  // unsaved edits pending in the tabs
    config::EngineConfig ui_;  // working copy, refreshed from config on Show

    // Library folder browsing (panel thread only).
    std::wstring              libraryFolder_;
    std::vector<std::wstring> libraryFiles_;
    int                       librarySelected_ = -1;

    // Preview thumbnail of the staged video (Shell-extracted).
    ID3D11ShaderResourceView* thumbSrv_ = nullptr;
    int                       thumbW_ = 0;
    int                       thumbH_ = 0;

    // Theming assets (panel thread only).
    void*   bodyFont_ = nullptr;   // ImFont* (Segoe UI) — void* to keep imgui out of the header
    void*   titleFont_ = nullptr;  // ImFont* (Segoe UI Bold, body size)
    void*   headerFont_ = nullptr; // ImFont* (Segoe UI Bold, large — section titles)
    ID3D11ShaderResourceView* logoSrv_ = nullptr;
    int     logoW_ = 0;
    int     logoH_ = 0;
    bool    comInit_ = false;
};

}  // namespace lwe::ui
