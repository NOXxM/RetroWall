cmake_minimum_required(VERSION 3.20)
project(RetroWall LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Live Wallpaper Engine — Phases 1-6
# C++20 / Win32 / D3D11 / DXGI / DirectComposition / Media Foundation / Dear ImGui.
# Targets Windows 10 (1703+) and Windows 11.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "RetroWall targets Windows 10/11 only.")
endif()

# --- Dear ImGui (vendored, pinned v1.91.6) as its own static lib so the app's
#     strict warning flags don't apply to third-party code. -------------------
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)
target_link_libraries(imgui PUBLIC
    d3d11 dxgi d3dcompiler dwmapi imm32
)

# --- Embedded resources (app icon + RW logo). The .rc is configured with the
#     absolute assets path so the resource compiler finds the files regardless
#     of its working directory. -------------------------------------------------
set(LWE_ASSETS "${CMAKE_SOURCE_DIR}/assets")
configure_file(src/app.rc.in "${CMAKE_BINARY_DIR}/app.rc" @ONLY)

add_executable(RetroWall WIN32
    src/main.cpp
    src/config/AutoStart.cpp
    src/config/ConfigManager.cpp
    src/desktop/DesktopAttach.cpp
    src/desktop/OcclusionProbe.cpp
    src/render/VideoRenderer.cpp
    src/render/DirtyRectTest.cpp
    src/media/VideoPlayer.cpp
    src/ui/SystemTrayManager.cpp
    src/ui/SettingsPanel.cpp
    src/engine/Engine.cpp
    "${CMAKE_BINARY_DIR}/app.rc"
)

target_include_directories(RetroWall PRIVATE src)

target_compile_definitions(RetroWall PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX                 # keep std::min / std::max usable
    WINVER=0x0A00            # Windows 10
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000003 # NTDDI_WIN10_RS2 (1703): per-monitor-v2 DPI API
)

# Win32 + graphics + Media Foundation + ImGui import libraries.
target_link_libraries(RetroWall PRIVATE
    imgui          # vendored Dear ImGui (+ win32/dx11 backends)
    d3d11
    dxgi
    dcomp          # DirectComposition
    d3dcompiler    # runtime HLSL compile
    mfplat         # MFStartup, MFCreateDXGIDeviceManager, media-type helpers
    mfreadwrite    # IMFSourceReader
    mfuuid         # MF GUIDs
    dwmapi
    user32
    gdi32
    ole32          # CoInitializeEx, SHGetKnownFolderPath/CoTaskMemFree
    shell32        # CommandLineToArgvW, Shell_NotifyIconW, SHGetKnownFolderPath
    advapi32       # registry auto-start
    comdlg32       # GetOpenFileNameW (video picker)
    uuid           # FOLDERID_RoamingAppData (KNOWNFOLDERID GUID)
    windowscodecs  # WIC: decode the embedded RW logo PNG into a D3D texture
)

if(MSVC)
    target_compile_options(RetroWall PRIVATE
        /W4 /permissive- /EHsc /utf-8
    )
    set_target_properties(RetroWall PROPERTIES
        VS_GLOBAL_EnableUnitySupport false
    )
endif()
