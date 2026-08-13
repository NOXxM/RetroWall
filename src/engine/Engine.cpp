#include "engine/Engine.hpp"

#include <dwmapi.h>
#include <objbase.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

#include "config/AutoStart.hpp"
#include "desktop/DesktopAttach.hpp"
#include "desktop/OcclusionProbe.hpp"
#include "media/FramePacer.hpp"
#include "media/VideoPlayer.hpp"
#include "render/VideoRenderer.hpp"
#include "win32/Error.hpp"
#include "win32/Log.hpp"
#include "win32/Resources.hpp"

namespace lwe::engine {

using win32::Win32Error;

Engine* Engine::s_instance = nullptr;

namespace {

constexpr wchar_t kControlClassName[] = L"LWE_ControlWindow";
constexpr wchar_t kRenderClassName[] = L"LWE_WallpaperHostWindow";

constexpr UINT kRenderErrorMsg = WM_APP + 0x10;  // fatal: show + quit
constexpr UINT kNotifyErrorMsg = WM_APP + 0x11;  // non-fatal: show, keep running
constexpr UINT kApplyMonitorMsg = WM_APP + 0x12; // wparam = monitor index
constexpr UINT kEvalScheduleMsg = WM_APP + 0x13; // re-run rotation/schedule now
constexpr UINT_PTR kScheduleTimerId = 1;         // periodic schedule tick
constexpr UINT kScheduleTickMs = 15000;          // 15s granularity

// EnumDisplayMonitors callback: collect each monitor's virtual-screen rect.
BOOL CALLBACK CollectMonitorRects(HMONITOR mon, HDC, LPRECT, LPARAM lparam) {
    auto* rects = reinterpret_cast<std::vector<RECT>*>(lparam);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(mon, &mi)) {
        rects->push_back(mi.rcMonitor);
    }
    return TRUE;
}

// Enumerate video files under a folder (recursive, capped, sorted).
std::vector<std::wstring> ListVideosInFolder(const std::wstring& folder) {
    std::vector<std::wstring> out;
    if (folder.empty()) return out;
    static const wchar_t* kExts[] = {
        L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi", L".wmv", L".asf",
        L".flv", L".ts",  L".m2ts", L".mts", L".mpg", L".mpeg", L".3gp", L".ogv"};
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        fs::path(folder), fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; it != end && out.size() < 1000; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::wstring ext = it->path().extension().wstring();
        for (const wchar_t* e : kExts) {
            if (::_wcsicmp(ext.c_str(), e) == 0) {
                out.push_back(it->path().wstring());
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Effective local UTC offset in hours (accounts for DST).
double LocalUtcOffsetHours() {
    TIME_ZONE_INFORMATION tzi{};
    const DWORD r = ::GetTimeZoneInformation(&tzi);
    long bias = tzi.Bias;
    if (r == TIME_ZONE_ID_DAYLIGHT) bias += tzi.DaylightBias;
    else if (r == TIME_ZONE_ID_STANDARD) bias += tzi.StandardBias;
    return -static_cast<double>(bias) / 60.0;  // local = UTC - Bias
}

// Sunrise/sunset (Almanac algorithm) -> minutes since local midnight.
// Returns false at extreme latitudes where the sun doesn't rise/set that day.
bool ComputeSunTimes(int year, int month, int day, double lat, double lng,
                     double tzHours, int& sunriseMin, int& sunsetMin) {
    constexpr double kPi = 3.14159265358979323846;
    const double D2R = kPi / 180.0, R2D = 180.0 / kPi;
    const double zenith = 90.833 * D2R;  // official (with refraction)

    const int N1 = 275 * month / 9;
    const int N2 = (month + 9) / 12;
    const int N3 = 1 + (year - 4 * (year / 4) + 2) / 3;
    const int N = N1 - (N2 * N3) + day - 30;  // day of year

    auto solve = [&](bool rising, double& outHour) -> bool {
        const double lngHour = lng / 15.0;
        const double t = rising ? (N + ((6 - lngHour) / 24))
                                : (N + ((18 - lngHour) / 24));
        const double M = (0.9856 * t) - 3.289;
        double L = M + (1.916 * std::sin(M * D2R)) +
                   (0.020 * std::sin(2 * M * D2R)) + 282.634;
        L = std::fmod(L + 360.0, 360.0);
        double RA = R2D * std::atan(0.91764 * std::tan(L * D2R));
        RA = std::fmod(RA + 360.0, 360.0);
        const double Lquad = std::floor(L / 90.0) * 90.0;
        const double RAquad = std::floor(RA / 90.0) * 90.0;
        RA = (RA + (Lquad - RAquad)) / 15.0;
        const double sinDec = 0.39782 * std::sin(L * D2R);
        const double cosDec = std::cos(std::asin(sinDec));
        const double cosH = (std::cos(zenith) - (sinDec * std::sin(lat * D2R))) /
                            (cosDec * std::cos(lat * D2R));
        if (cosH > 1.0 || cosH < -1.0) return false;  // no sunrise/sunset
        double H = rising ? (360.0 - R2D * std::acos(cosH))
                          : (R2D * std::acos(cosH));
        H /= 15.0;
        const double T = H + RA - (0.06571 * t) - 6.622;
        const double UT = std::fmod(T - lngHour + 24.0, 24.0);
        outHour = std::fmod(UT + tzHours + 24.0, 24.0);
        return true;
    };

    double sr = 0, ss = 0;
    if (!solve(true, sr) || !solve(false, ss)) return false;
    sunriseMin = static_cast<int>(std::lround(sr * 60.0));
    sunsetMin = static_cast<int>(std::lround(ss * 60.0));
    return true;
}

// Heuristic: is a known screen-recording app running? (Best-effort only.)
bool IsCaptureAppRunning() {
    static const wchar_t* kApps[] = {L"obs64.exe", L"obs32.exe", L"obs.exe",
                                     L"bdcam.exe", L"Camtasia.exe",
                                     L"CamtasiaStudio.exe"};
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (::Process32FirstW(snap, &pe)) {
        do {
            for (const wchar_t* a : kApps) {
                if (::_wcsicmp(pe.szExeFile, a) == 0) { found = true; break; }
            }
        } while (!found && ::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return found;
}

// GUID_ACDC_POWER_SOURCE {5D3E9A59-E9D5-4B00-A6BD-FF34FF516548}
const GUID kAcDcPowerSource = {
    0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};
// GUID_POWER_SAVING_STATUS {E00958C0-C213-4ACE-AC77-FECCED2EEEA5}
const GUID kPowerSavingStatus = {
    0xe00958c0, 0xc213, 0x4ace, {0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5}};

}  // namespace

// ---------------------------------------------------------------------------
// Construction / teardown
// ---------------------------------------------------------------------------
Engine::Engine(std::wstring videoPath, bool openSettings)
    : videoPath_(std::move(videoPath)), openSettings_(openSettings) {
    instance_ = ::GetModuleHandleW(nullptr);

    BOOL compositionEnabled = FALSE;
    if (SUCCEEDED(::DwmIsCompositionEnabled(&compositionEnabled)) &&
        compositionEnabled == FALSE) {
        throw std::runtime_error(
            "DWM composition is disabled; DirectComposition is required.");
    }

    CreateControlWindow();
    CreateRenderWindow();

    const desktop::DesktopHost host = desktop::AcquireDesktopHost();
    desktop::AttachToDesktop(renderWnd_, host);

    config_.Load();
    // A command-line path overrides (and persists into) the saved config. This
    // runs before observers are registered, so it just seeds the value.
    if (!videoPath_.empty()) {
        config_.SetVideoPath(videoPath_);
    }

    // Honor a saved monitor target immediately (the surface was just attached
    // spanning all monitors; narrow it to the configured display if any).
    ApplyMonitorTarget(config_.Snapshot().monitorIndex);

    s_instance = this;
    SetupUi();
    RegisterPowerNotifications();
    InstallWinEventHook();
    EvaluatePowerState();

    // Scheduling timer + immediate first evaluation.
    ::SetTimer(controlWnd_, kScheduleTimerId, kScheduleTickMs, nullptr);
    EvaluateSchedule();  // apply rotation / day-night immediately at boot
}

Engine::~Engine() {
    if (controlWnd_ != nullptr) {
        ::KillTimer(controlWnd_, kScheduleTimerId);
    }
    RequestShutdown();
    JoinThreads();          // render thread

    settingsPanel_.reset();  // stops + joins the panel thread; releases its D3D
    tray_.reset();           // removes the icon; destroys the tray window
    config_.Save();

    RemoveWinEventHook();
    UnregisterPowerNotifications();
    s_instance = nullptr;

    if (renderWnd_ != nullptr) {
        ::DestroyWindow(renderWnd_);
        renderWnd_ = nullptr;
    }
    if (controlWnd_ != nullptr) {
        ::DestroyWindow(controlWnd_);
        controlWnd_ = nullptr;
    }
    if (renderClass_ != 0) {
        ::UnregisterClassW(MAKEINTATOM(renderClass_), instance_);
        renderClass_ = 0;
    }
    if (controlClass_ != 0) {
        ::UnregisterClassW(MAKEINTATOM(controlClass_), instance_);
        controlClass_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------
void Engine::CreateControlWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Engine::ControlThunk;
    wc.hInstance = instance_;
    wc.lpszClassName = kControlClassName;
    controlClass_ = ::RegisterClassExW(&wc);
    if (controlClass_ == 0) {
        throw Win32Error(static_cast<long>(::GetLastError()),
                         "RegisterClassEx(control)");
    }
    controlWnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW, MAKEINTATOM(controlClass_), L"LiveWallpaperEngine",
        WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, instance_, this);
    if (controlWnd_ == nullptr) {
        throw Win32Error(static_cast<long>(::GetLastError()),
                         "CreateWindowEx(control)");
    }
}

void Engine::CreateRenderWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Engine::RenderThunk;
    wc.hInstance = instance_;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kRenderClassName;
    renderClass_ = ::RegisterClassExW(&wc);
    if (renderClass_ == 0) {
        throw Win32Error(static_cast<long>(::GetLastError()),
                         "RegisterClassEx(render)");
    }

    const int x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    renderWnd_ = ::CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        MAKEINTATOM(renderClass_), L"LiveWallpaperEngineSurface", WS_POPUP, x, y,
        w, h, nullptr, nullptr, instance_, this);
    if (renderWnd_ == nullptr) {
        throw Win32Error(static_cast<long>(::GetLastError()),
                         "CreateWindowEx(render)");
    }
    ::ShowWindow(renderWnd_, SW_SHOWNOACTIVATE);
}

LRESULT CALLBACK Engine::ControlThunk(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
    Engine* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<Engine*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->controlWnd_ = hwnd;
    } else {
        self = reinterpret_cast<Engine*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->ControlProc(hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK Engine::RenderThunk(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
    Engine* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<Engine*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->renderWnd_ = hwnd;
    } else {
        self = reinterpret_cast<Engine*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->RenderProc(hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT Engine::ControlProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_POWERBROADCAST:
            switch (wparam) {
                case PBT_APMSUSPEND: {
                    {
                        std::scoped_lock<std::mutex> lock(stateMutex_);
                        powerFreeze_ = true;
                        wake_ = true;
                    }
                    stateCv_.notify_all();
                    break;
                }
                case PBT_APMRESUMESUSPEND:
                case PBT_APMRESUMEAUTOMATIC:
                case PBT_APMPOWERSTATUSCHANGE:
                case PBT_POWERSETTINGCHANGE:
                    EvaluatePowerState();
                    break;
                default:
                    break;
            }
            return TRUE;

        case WM_DISPLAYCHANGE:
            // Resolution / monitor arrangement changed: re-apply the target so
            // the surface stays on the configured display at the new geometry.
            ApplyMonitorTarget(config_.Snapshot().monitorIndex);
            return 0;

        case kApplyMonitorMsg:
            ApplyMonitorTarget(static_cast<int>(wparam));
            return 0;

        case WM_TIMER:
            if (wparam == kScheduleTimerId) EvaluateSchedule();
            return 0;

        case kEvalScheduleMsg:
            EvaluateSchedule();
            return 0;

        case kRenderErrorMsg: {
            std::string message;
            {
                std::scoped_lock<std::mutex> lock(stateMutex_);
                message = renderError_;
            }
            ::MessageBoxA(nullptr, message.c_str(),
                          "RetroWall — playback error",
                          MB_ICONERROR | MB_OK);
            ::DestroyWindow(hwnd);
            return 0;
        }

        case kNotifyErrorMsg: {
            // Non-fatal (e.g. couldn't switch to a newly-picked video): inform
            // the user but keep the engine running.
            std::string message;
            {
                std::scoped_lock<std::mutex> lock(stateMutex_);
                message = renderError_;
            }
            ::MessageBoxA(nullptr, message.c_str(), "RetroWall",
                          MB_ICONWARNING | MB_OK);
            return 0;
        }

        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            controlWnd_ = nullptr;
            ::PostQuitMessage(0);
            return 0;

        default:
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

LRESULT Engine::RenderProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_SIZE: {
            const std::uint32_t width = LOWORD(lparam);
            const std::uint32_t height = HIWORD(lparam);
            if (width != 0 && height != 0) {
                pendingResize_.store(
                    (static_cast<std::uint64_t>(width) << 32) | height,
                    std::memory_order_release);
            }
            return 0;
        }
        case WM_DESTROY:
            renderWnd_ = nullptr;
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

// ---------------------------------------------------------------------------
// UI wiring + live config observer
// ---------------------------------------------------------------------------
void Engine::SetupUi() {
    ui::SystemTrayManager::Callbacks tcb;
    tcb.onPlayPause = [this] { SetManualPause(tray_->IsPaused()); };
    tcb.onMuteToggle = [this] { config_.SetMuted(tray_->IsMuted()); };
    tcb.onOpenSettings = [this] {
        if (settingsPanel_) settingsPanel_->Show();
    };
    tcb.onExit = [this] { RequestExit(); };
    // Load the embedded RetroWall icon at tray size (16px) for a crisp notify icon.
    HICON trayIcon = static_cast<HICON>(::LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0));
    tray_ = std::make_unique<ui::SystemTrayManager>(
        instance_, L"RetroWall", trayIcon, std::move(tcb));

    settingsPanel_ = std::make_unique<ui::SettingsPanel>(instance_, config_);

    config_.AddObserver(
        [this](const config::EngineConfig& c, config::ConfigField f) {
            OnConfigChanged(c, f);
        });

    tray_->SetMuted(config_.Muted());  // reflect persisted mute state

    if (openSettings_ && settingsPanel_) {
        settingsPanel_->Show();  // --settings: pop the panel on launch
    }
}

void Engine::OnConfigChanged(const config::EngineConfig& c,
                             config::ConfigField field) {
    using F = config::ConfigField;
    switch (field) {
        case F::StartWithWindows:
            config::SetAutoStart(c.startWithWindows, AutoStartCommandLine());
            break;
        case F::Muted:
            if (tray_) tray_->SetMuted(c.muted);
            SignalWake();
            break;
        case F::VideoPath: {
            std::scoped_lock<std::mutex> lock(stateMutex_);
            pendingVideoPath_ = c.videoPath;
            videoReload_ = true;
            wake_ = true;
            stateCv_.notify_all();
            break;
        }
        case F::Monitor:
            // Window work must happen on the pump thread that owns renderWnd_.
            if (controlWnd_ != nullptr) {
                ::PostMessageW(controlWnd_, kApplyMonitorMsg,
                               static_cast<WPARAM>(c.monitorIndex), 0);
            }
            break;
        case F::Rotation:
        case F::Schedule:
            // Re-evaluate on the pump thread so a settings change applies at once.
            if (controlWnd_ != nullptr) {
                ::PostMessageW(controlWnd_, kEvalScheduleMsg, 0, 0);
            }
            break;
        case F::Privacy:
            SignalWake();  // blackout is read per-frame from the atomic
            break;
        case F::All:
            if (tray_) tray_->SetMuted(c.muted);
            SignalWake();
            break;
        default:
            // TargetFps / PlaybackSpeed / MemoryEviction / PauseRules / Volume /
            // Color (post-processing) are read from atomics by the render thread;
            // just nudge it to re-evaluate promptly.
            SignalWake();
            break;
    }
}

// ---------------------------------------------------------------------------
// Monitor targeting
//
// The render window is a child of the WorkerW wallpaper layer, whose client
// origin (0,0) maps to the virtual-screen origin. So a monitor's rect in
// virtual-screen coordinates becomes child coordinates by subtracting the
// virtual origin. monitorIndex 0 = span everything; 1..N = that physical
// monitor (ordered left-to-right, then top-to-bottom).
// ---------------------------------------------------------------------------
void Engine::ApplyMonitorTarget(int monitorIndex) {
    if (renderWnd_ == nullptr) {
        return;
    }
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    int x = 0, y = 0, w = vw, h = vh;  // default: cover the whole virtual desktop
    if (monitorIndex > 0) {
        std::vector<RECT> mons;
        ::EnumDisplayMonitors(nullptr, nullptr, &CollectMonitorRects,
                              reinterpret_cast<LPARAM>(&mons));
        std::sort(mons.begin(), mons.end(), [](const RECT& a, const RECT& b) {
            return (a.left != b.left) ? (a.left < b.left) : (a.top < b.top);
        });
        const size_t idx = static_cast<size_t>(monitorIndex - 1);
        if (idx < mons.size()) {
            const RECT& m = mons[idx];
            x = m.left - vx;
            y = m.top - vy;
            w = m.right - m.left;
            h = m.bottom - m.top;
        }
    }

    ::SetWindowPos(renderWnd_, HWND_BOTTOM, x, y, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // Repositioning fires WM_SIZE -> pendingResize_, but set it explicitly too so
    // the swapchain is guaranteed to match even if WM_SIZE is coalesced/dropped.
    if (w > 0 && h > 0) {
        pendingResize_.store((static_cast<std::uint64_t>(static_cast<std::uint32_t>(w)) << 32) |
                                 static_cast<std::uint32_t>(h),
                             std::memory_order_release);
        SignalWake();
    }
    log::Writef(L"ApplyMonitorTarget idx=%d -> child %d,%d %dx%d", monitorIndex, x,
                y, w, h);
}

// ---------------------------------------------------------------------------
// Rotation + day/night scheduling (pump thread)
// ---------------------------------------------------------------------------
void Engine::EvaluateSchedule() {
    const config::EngineConfig c = config_.Snapshot();
    std::wstring desired;
    bool haveDesired = false;

    if (c.scheduleMode != 0) {
        // Day/Night: pick the clip for the current phase.
        SYSTEMTIME lt{};
        ::GetLocalTime(&lt);
        const int nowMin = lt.wHour * 60 + lt.wMinute;
        bool daytime = true;
        if (c.scheduleMode == 1) {  // fixed times
            if (c.dayStartMinutes <= c.nightStartMinutes) {
                daytime = (nowMin >= c.dayStartMinutes && nowMin < c.nightStartMinutes);
            } else {  // day window wraps past midnight
                daytime = (nowMin >= c.dayStartMinutes || nowMin < c.nightStartMinutes);
            }
        } else {  // astronomical
            int sr = 0, ss = 0;
            if (ComputeSunTimes(lt.wYear, lt.wMonth, lt.wDay, c.latitude, c.longitude,
                                LocalUtcOffsetHours(), sr, ss)) {
                daytime = (nowMin >= sr && nowMin < ss);
            }  // else polar day/night -> default daytime
        }
        desired = daytime ? c.dayVideoPath : c.nightVideoPath;
        haveDesired = !desired.empty();
    } else if (c.rotationEnabled && !c.rotationFolder.empty()) {
        // Playlist rotation: advance when the interval has elapsed.
        const long long now = static_cast<long long>(::time(nullptr));
        const long long interval =
            static_cast<long long>(std::max(1, c.rotationIntervalMinutes)) * 60;
        if (c.rotationLastSwitch == 0 || (now - c.rotationLastSwitch) >= interval) {
            const std::vector<std::wstring> files = ListVideosInFolder(c.rotationFolder);
            if (!files.empty()) {
                size_t idx = 0;
                for (size_t i = 0; i < files.size(); ++i) {
                    if (files[i] == c.videoPath) { idx = i; break; }
                }
                const size_t next =
                    (c.rotationLastSwitch == 0) ? 0 : (idx + 1) % files.size();
                desired = files[next];
                haveDesired = true;
                config_.SetRotationLastSwitch(now);  // persists across restarts
            }
        }
    }

    if (haveDesired && desired != config_.Snapshot().videoPath) {
        config_.SetVideoPath(desired);
    }
}

// ---------------------------------------------------------------------------
// Power management
// ---------------------------------------------------------------------------
void Engine::RegisterPowerNotifications() {
    powerAcDc_ = ::RegisterPowerSettingNotification(
        controlWnd_, &kAcDcPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);
    powerSaver_ = ::RegisterPowerSettingNotification(
        controlWnd_, &kPowerSavingStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void Engine::UnregisterPowerNotifications() {
    if (powerAcDc_ != nullptr) {
        ::UnregisterPowerSettingNotification(powerAcDc_);
        powerAcDc_ = nullptr;
    }
    if (powerSaver_ != nullptr) {
        ::UnregisterPowerSettingNotification(powerSaver_);
        powerSaver_ = nullptr;
    }
}

void Engine::EvaluatePowerState() {
    SYSTEM_POWER_STATUS sps{};
    if (!::GetSystemPowerStatus(&sps)) {
        return;
    }
    const bool onBattery = (sps.ACLineStatus == 0);
    const bool batterySaver = (sps.SystemStatusFlag & 0x01) != 0;
    const bool freeze = onBattery || batterySaver;
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        powerFreeze_ = freeze;
        wake_ = true;
    }
    stateCv_.notify_all();
}

// ---------------------------------------------------------------------------
// Occlusion notification
// ---------------------------------------------------------------------------
void Engine::InstallWinEventHook() {
    winEventHook_ = ::SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr,
        &Engine::WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (winEventHook_ == nullptr) {
        ::OutputDebugStringW(
            L"[LWE] SetWinEventHook failed; occlusion wake-ups disabled\n");
    }
}

void Engine::RemoveWinEventHook() {
    if (winEventHook_ != nullptr) {
        ::UnhookWinEvent(winEventHook_);
        winEventHook_ = nullptr;
    }
}

void Engine::WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (s_instance != nullptr) {
        s_instance->OnDesktopChanged();
    }
}

void Engine::OnDesktopChanged() { SignalWake(); }

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------
void Engine::SetManualPause(bool paused) {
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        manualPause_ = paused;
        wake_ = true;
    }
    stateCv_.notify_all();
}

void Engine::RequestExit() {
    RequestShutdown();
    if (controlWnd_ != nullptr) {
        ::DestroyWindow(controlWnd_);
    }
}

void Engine::SignalWake() {
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        wake_ = true;
    }
    stateCv_.notify_all();
}

void Engine::RequestShutdown() {
    {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        running_ = false;
        wake_ = true;
    }
    stateCv_.notify_all();
}

void Engine::EvictWorkingSet() {
    if (::SetProcessWorkingSetSize(::GetCurrentProcess(),
                                   static_cast<SIZE_T>(-1),
                                   static_cast<SIZE_T>(-1)) == 0) {
        ::OutputDebugStringW(L"[LWE] SetProcessWorkingSetSize eviction failed\n");
    }
}

void Engine::JoinThreads() {
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
}

std::wstring Engine::AutoStartCommandLine() const {
    std::wstring exe(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            ::GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (n == 0) { exe.clear(); break; }
        if (n < exe.size()) { exe.resize(n); break; }
        exe.resize(exe.size() * 2);
    }
    std::wstring cmd = L"\"";
    cmd += exe;
    cmd += L"\"";
    const std::wstring path = config_.Snapshot().videoPath;
    if (!path.empty()) {
        cmd += L" \"";
        cmd += path;
        cmd += L"\"";
    }
    return cmd;
}

// ---------------------------------------------------------------------------
// Run loop (pump thread)
// ---------------------------------------------------------------------------
int Engine::Run() {
    renderThread_ = std::thread(&Engine::RenderThreadMain, this);

    MSG msg{};
    BOOL rc = 0;
    while ((rc = ::GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (rc == -1) break;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    RequestShutdown();
    JoinThreads();
    config_.Save();
    return static_cast<int>(msg.wParam);
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
void Engine::RenderThreadMain() {
    const bool comReady = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    try {
        render::VideoRenderer renderer(renderWnd_);

        std::unique_ptr<media::VideoPlayer> player;
        std::wstring currentPath = config_.Snapshot().videoPath;
        if (!currentPath.empty()) {
            log::Writef(L"Initial video path: %s", currentPath.c_str());
            player = std::make_unique<media::VideoPlayer>(renderer.Device(),
                                                          currentPath);
        } else {
            log::Write(L"No initial video path; idle until one is chosen.");
        }

        media::FramePacer pacer;
        bool occluded = false;
        bool foreground = false;
        bool frozenPrev = false;
        bool loggedFirstPresent = false;
        int consecutiveLoops = 0;
        float lastSpeed = 1.0f;
        int themeCounter = 0;     // throttle the system light/dark registry read
        bool sysDark = false;
        int captureCounter = 0;   // throttle the screen-capture process poll
        bool captureActive = false;
        int lastLayout = -1;      // recompute monitor layout when these change
        int lastMonitor = -2;
        LONGLONG lastPresentedMedia = -1;

        for (;;) {
            bool run = true, manual = false, power = false, doProbe = false;
            bool reload = false;
            std::wstring newPath;
            {
                std::scoped_lock<std::mutex> lock(stateMutex_);
                run = running_;
                manual = manualPause_;
                power = powerFreeze_;
                doProbe = wake_;
                wake_ = false;
                reload = videoReload_;
                videoReload_ = false;
                if (reload) newPath = pendingVideoPath_;
            }
            if (!run) break;

            if (reload) {
                // Swap the clip without killing the app if the new path is bad.
                log::Writef(L"Reloading video: %s", newPath.c_str());
                try {
                    player = std::make_unique<media::VideoPlayer>(
                        renderer.Device(), newPath);
                    currentPath = newPath;
                    pacer.Reset();
                    lastPresentedMedia = -1;
                    consecutiveLoops = 0;
                    log::Write(L"Reload OK");
                } catch (const std::exception& e) {
                    // Keep the previous clip, but TELL the user why (e.g. an
                    // unsupported format) instead of failing silently.
                    ::OutputDebugStringA(e.what());
                    log::Write(L"Reload FAILED (see message box)");
                    {
                        std::scoped_lock<std::mutex> lock(stateMutex_);
                        renderError_ = e.what();
                    }
                    if (controlWnd_ != nullptr) {
                        ::PostMessageW(controlWnd_, kNotifyErrorMsg, 0, 0);
                    }
                }
            }

            if (doProbe) {
                occluded = desktop::IsWallpaperOccluded(renderWnd_);
                foreground = desktop::IsForegroundAppActive();
            }

            const bool freezeOccl =
                occluded && (config_.PauseOnMaximized() || config_.PauseOnFullscreen());
            const bool freezeFocus = foreground && config_.PauseOnFocused();
            const bool freezePower = power && config_.PauseOnBattery();
            const bool frozen = manual || freezeOccl || freezeFocus ||
                                freezePower || (player == nullptr);

            if (frozen) {
                if (!frozenPrev) {
                    log::Writef(
                        L"Frozen: manual=%d occl=%d focus=%d power=%d noPlayer=%d",
                        manual ? 1 : 0, freezeOccl ? 1 : 0, freezeFocus ? 1 : 0,
                        freezePower ? 1 : 0, (player == nullptr) ? 1 : 0);
                    if (config_.MemoryEviction()) EvictWorkingSet();
                    frozenPrev = true;
                }
                std::unique_lock<std::mutex> lock(stateMutex_);
                stateCv_.wait(lock, [this] { return wake_ || !running_; });
                continue;
            }
            if (frozenPrev) {
                pacer.Reset();
                lastPresentedMedia = -1;
                frozenPrev = false;
            }

            if (const std::uint64_t packed =
                    pendingResize_.exchange(0, std::memory_order_acquire);
                packed != 0) {
                renderer.Resize(static_cast<std::uint32_t>(packed >> 32),
                                static_cast<std::uint32_t>(packed & 0xFFFFFFFFu));
            }

            float speed = config_.PlaybackSpeed();
            if (speed < 0.05f) speed = 0.05f;
            if (speed != lastSpeed) {
                pacer.Reset();
                lastPresentedMedia = -1;
                lastSpeed = speed;
            }

            const media::VideoFrame f = player->NextFrame();
            if (f.looped) {
                if (++consecutiveLoops > 4) {
                    throw std::runtime_error(
                        "Video contains no decodable frames (unsupported codec "
                        "or empty stream).");
                }
                pacer.Reset();
                lastPresentedMedia = -1;
                continue;
            }
            if (!f.valid) {
                // Rare transient (stream tick / dropped sample). Back off briefly
                // so a burst of these can't spin the thread at 100% CPU.
                ::Sleep(2);
                continue;
            }
            consecutiveLoops = 0;

            // Pace to (speed-scaled) media time -> real time. Kernel wait, no spin.
            const LONGLONG scaled =
                static_cast<LONGLONG>(static_cast<double>(f.timeStamp) / speed);
            pacer.WaitForFrame(scaled);

            // Frame limiter: drop presents that arrive faster than the target FPS
            // (decode still runs at source rate; this caps GPU/present work).
            const int fps = config_.TargetFps() > 0 ? config_.TargetFps() : 60;
            const LONGLONG minDelta = 10'000'000LL / fps;
            const bool due = (lastPresentedMedia < 0) ||
                             (f.timeStamp - lastPresentedMedia >= minDelta);
            if (!due) {
                continue;
            }

            // Update the multi-monitor layout when the mode or target changes.
            const int curLayout = config_.Layout();
            const int curMonitor = config_.MonitorIndex();
            if (curLayout != lastLayout || curMonitor != lastMonitor) {
                lastLayout = curLayout;
                lastMonitor = curMonitor;
                std::vector<render::VideoRenderer::MonitorRect> rects;
                if (curMonitor == 0 && curLayout != 1 /*not Stretch*/) {
                    std::vector<RECT> mons;
                    ::EnumDisplayMonitors(nullptr, nullptr, &CollectMonitorRects,
                                          reinterpret_cast<LPARAM>(&mons));
                    if (mons.size() > 1) {
                        const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
                        const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
                        for (const RECT& m : mons) {
                            rects.push_back(render::VideoRenderer::MonitorRect{
                                m.left - vx, m.top - vy, m.right - m.left,
                                m.bottom - m.top});
                        }
                    }
                }
                renderer.SetMonitorLayout(curLayout, std::move(rects));
            }

            // Push the live color-grade params to the shader before presenting.
            render::VideoRenderer::PostParams pp;
            pp.brightness = config_.Brightness();
            pp.contrast = config_.Contrast();
            pp.saturation = config_.Saturation();
            pp.gamma = config_.Gamma();
            pp.tintR = config_.TintR();
            pp.tintG = config_.TintG();
            pp.tintB = config_.TintB();
            pp.temperature = config_.Temperature();
            pp.aspectMode = config_.Aspect();  // 0 Fill, 1 Fit, 2 Stretch
            if (config_.MatchSystemTheme()) {
                if (themeCounter-- <= 0) {
                    themeCounter = 120;  // re-check ~every 2s at 60fps
                    DWORD light = 1, sz = sizeof(light);
                    HKEY key = nullptr;
                    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                            0, KEY_READ, &key) == ERROR_SUCCESS) {
                        ::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                                           reinterpret_cast<LPBYTE>(&light), &sz);
                        ::RegCloseKey(key);
                    }
                    sysDark = (light == 0);
                }
                pp.temperature += sysDark ? 0.25f : -0.10f;  // dark->cooler, light->warmer
                if (sysDark) pp.brightness *= 0.92f;
            }
            // Privacy blackout: if a recorder is detected, render solid black.
            if (config_.BlackoutOnCapture()) {
                if (captureCounter-- <= 0) {
                    captureCounter = 60;  // re-check ~once/sec
                    captureActive = IsCaptureAppRunning();
                }
                if (captureActive) pp.blackout = 1.0f;
            } else {
                captureActive = false;
            }
            renderer.SetPostParams(pp);

            // Flip-model path exposes a frame-latency waitable; the BitBlt
            // fallback does not (returns null) — the pacer already bounds timing.
            if (HANDLE waitable = renderer.FrameLatencyWaitable()) {
                ::WaitForSingleObject(waitable, 1000);
            }
            renderer.PresentFrame(f.texture.Get(), f.subresource, f.width,
                                  f.height);
            lastPresentedMedia = f.timeStamp;
            if (!loggedFirstPresent) {
                log::Writef(L"First frame presented: %ux%u", f.width, f.height);
                loggedFirstPresent = true;
            }
        }
    } catch (const std::exception& e) {
        ::OutputDebugStringA(e.what());
        {
            std::scoped_lock<std::mutex> lock(stateMutex_);
            renderError_ = e.what();
        }
        if (controlWnd_ != nullptr) {
            ::PostMessageW(controlWnd_, kRenderErrorMsg, 0, 0);
        }
    }

    if (comReady) {
        ::CoUninitialize();
    }
}

}  // namespace lwe::engine
