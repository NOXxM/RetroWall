// ---------------------------------------------------------------------------
// main.cpp — RetroWall
// Modes:
//   RetroWall.exe <path-to-video.mp4>   run the video wallpaper
//   RetroWall.exe --settings            run and open the settings panel
//   RetroWall.exe --dirty-rect-test     run the geometry self-test
// ---------------------------------------------------------------------------
#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <exception>
#include <string>

#include "engine/Engine.hpp"
#include "render/DirtyRect.hpp"
#include "win32/Error.hpp"

namespace {

void AttachConsole() {
    if (::AllocConsole()) {
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
        FILE* err = nullptr;
        freopen_s(&err, "CONOUT$", "w", stderr);
    }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE /*instance*/, HINSTANCE /*prevInstance*/,
                      PWSTR /*cmdLine*/, int /*showCmd*/) {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    std::wstring videoPath;
    bool runTest = false;
    bool openSettings = false;
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--dirty-rect-test") {
                runTest = true;
            } else if (arg == L"--settings") {
                openSettings = true;
            } else if (!arg.empty() && arg[0] != L'-') {
                videoPath = arg;
            }
        }
        ::LocalFree(argv);
    }

    if (runTest) {
        AttachConsole();
        return lwe::render::RunDirtyRectSelfTest();
    }

    // A video path is optional now: with none given the engine falls back to the
    // saved config, and if that is empty too it starts idle (tray + settings)
    // until the user picks a clip in the Settings panel.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    try {
        lwe::engine::Engine engine(videoPath, openSettings);
        return engine.Run();
    } catch (const lwe::win32::Win32Error& e) {
        ::OutputDebugStringA(e.what());
        ::MessageBoxA(nullptr, e.what(),
                      "RetroWall — fatal Win32 error",
                      MB_ICONERROR | MB_OK);
        return 1;
    } catch (const std::exception& e) {
        ::OutputDebugStringA(e.what());
        ::MessageBoxA(nullptr, e.what(), "RetroWall — fatal",
                      MB_ICONERROR | MB_OK);
        return 1;
    }
}
