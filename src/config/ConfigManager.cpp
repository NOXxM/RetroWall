#include "config/ConfigManager.hpp"

#include <shlobj.h>

#include <cwchar>
#include <cstdlib>

namespace lwe::config {

namespace {

constexpr wchar_t kSecLibrary[] = L"Library";
constexpr wchar_t kSecDisplay[] = L"Display";
constexpr wchar_t kSecPerf[] = L"Performance";
constexpr wchar_t kSecAudio[] = L"Audio";
constexpr wchar_t kSecSystem[] = L"System";
constexpr wchar_t kSecColor[] = L"Color";
constexpr wchar_t kSecRotation[] = L"Rotation";
constexpr wchar_t kSecSchedule[] = L"Schedule";
constexpr wchar_t kSecPrivacy[] = L"Privacy";

int ReadInt(const wchar_t* sec, const wchar_t* key, int def,
            const std::wstring& path) {
    return static_cast<int>(::GetPrivateProfileIntW(sec, key, def, path.c_str()));
}

bool ReadBool(const wchar_t* sec, const wchar_t* key, bool def,
              const std::wstring& path) {
    return ::GetPrivateProfileIntW(sec, key, def ? 1 : 0, path.c_str()) != 0;
}

float ReadFloat(const wchar_t* sec, const wchar_t* key, float def,
                const std::wstring& path) {
    wchar_t defBuf[64];
    ::swprintf(defBuf, 64, L"%.4f", def);
    wchar_t buf[64] = {};
    ::GetPrivateProfileStringW(sec, key, defBuf, buf, 64, path.c_str());
    return static_cast<float>(::wcstod(buf, nullptr));
}

std::wstring ReadStr(const wchar_t* sec, const wchar_t* key,
                     const std::wstring& path) {
    wchar_t buf[2048] = {};
    ::GetPrivateProfileStringW(sec, key, L"", buf, 2048, path.c_str());
    return std::wstring(buf);
}

long long ReadInt64(const wchar_t* sec, const wchar_t* key, long long def,
                    const std::wstring& path) {
    wchar_t defBuf[32];
    ::swprintf(defBuf, 32, L"%lld", def);
    wchar_t buf[32] = {};
    ::GetPrivateProfileStringW(sec, key, defBuf, buf, 32, path.c_str());
    return ::_wcstoi64(buf, nullptr, 10);
}

void WriteInt64(const wchar_t* sec, const wchar_t* key, long long v,
                const std::wstring& path) {
    wchar_t b[32];
    ::swprintf(b, 32, L"%lld", v);
    ::WritePrivateProfileStringW(sec, key, b, path.c_str());
}

void WriteInt(const wchar_t* sec, const wchar_t* key, int v,
              const std::wstring& path) {
    wchar_t b[32];
    ::swprintf(b, 32, L"%d", v);
    ::WritePrivateProfileStringW(sec, key, b, path.c_str());
}

void WriteFloat(const wchar_t* sec, const wchar_t* key, float v,
                const std::wstring& path) {
    wchar_t b[64];
    ::swprintf(b, 64, L"%.4f", v);
    ::WritePrivateProfileStringW(sec, key, b, path.c_str());
}

void WriteStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v,
              const std::wstring& path) {
    ::WritePrivateProfileStringW(sec, key, v.c_str(), path.c_str());
}

}  // namespace

ConfigManager::ConfigManager() {
    ResolvePaths();
    // Atomics already default-constructed to match EngineConfig defaults; keep
    // them consistent with config_ from the start.
    std::unique_lock<std::shared_mutex> lock(mutex_);
    RefreshAtomics();
}

void ConfigManager::ResolvePaths() {
    PWSTR appData = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE,
                                         nullptr, &appData)) &&
        appData != nullptr) {
        iniDir_ = appData;
        iniDir_ += L"\\NativeWallpaperEngine";
    }
    if (appData != nullptr) {
        ::CoTaskMemFree(appData);
    }
    if (iniDir_.empty()) {
        iniDir_ = L".";  // fallback: current directory
    }
    iniPath_ = iniDir_ + L"\\config.ini";
}

void ConfigManager::Load() {
    // Auto-create the config directory.
    ::CreateDirectoryW(iniDir_.c_str(), nullptr);

    const DWORD attrs = ::GetFileAttributesW(iniPath_.c_str());
    const bool exists =
        (attrs != INVALID_FILE_ATTRIBUTES) &&
        ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (exists) {
            ReadFromIni();  // overlay saved values onto the defaults
        } else {
            config_ = EngineConfig{};  // first boot: fallback defaults
        }
        RefreshAtomics();
    }

    if (!exists) {
        Save();  // materialize defaults on first boot
    }
    Notify(ConfigField::All, Snapshot());
}

void ConfigManager::ReadFromIni() {
    // Caller holds the unique_lock. config_ starts at defaults.
    const std::wstring& p = iniPath_;
    EngineConfig& c = config_;

    const std::wstring path = ReadStr(kSecLibrary, L"VideoPath", p);
    if (!path.empty()) {
        c.videoPath = path;
    }

    c.monitorIndex = ReadInt(kSecDisplay, L"MonitorIndex", c.monitorIndex, p);
    c.layout = static_cast<LayoutMode>(
        ReadInt(kSecDisplay, L"Layout", static_cast<int>(c.layout), p));
    c.aspect = static_cast<AspectMode>(
        ReadInt(kSecDisplay, L"Aspect", static_cast<int>(c.aspect), p));

    c.targetFps = ReadInt(kSecPerf, L"TargetFps", c.targetFps, p);
    c.pauseOnMaximized = ReadBool(kSecPerf, L"PauseOnMaximized", c.pauseOnMaximized, p);
    c.pauseOnFullscreen = ReadBool(kSecPerf, L"PauseOnFullscreen", c.pauseOnFullscreen, p);
    c.pauseOnFocused = ReadBool(kSecPerf, L"PauseOnFocused", c.pauseOnFocused, p);
    c.pauseOnBattery = ReadBool(kSecPerf, L"PauseOnBattery", c.pauseOnBattery, p);

    c.volume = ReadFloat(kSecAudio, L"Volume", c.volume, p);
    c.smartMute = ReadBool(kSecAudio, L"SmartMute", c.smartMute, p);
    c.playbackSpeed = ReadFloat(kSecAudio, L"PlaybackSpeed", c.playbackSpeed, p);
    c.muted = ReadBool(kSecAudio, L"Muted", c.muted, p);

    c.startWithWindows = ReadBool(kSecSystem, L"StartWithWindows", c.startWithWindows, p);
    c.memoryEviction = ReadBool(kSecSystem, L"MemoryEviction", c.memoryEviction, p);

    c.brightness = ReadFloat(kSecColor, L"Brightness", c.brightness, p);
    c.contrast = ReadFloat(kSecColor, L"Contrast", c.contrast, p);
    c.saturation = ReadFloat(kSecColor, L"Saturation", c.saturation, p);
    c.gamma = ReadFloat(kSecColor, L"Gamma", c.gamma, p);
    c.tintR = ReadFloat(kSecColor, L"TintR", c.tintR, p);
    c.tintG = ReadFloat(kSecColor, L"TintG", c.tintG, p);
    c.tintB = ReadFloat(kSecColor, L"TintB", c.tintB, p);
    c.temperature = ReadFloat(kSecColor, L"Temperature", c.temperature, p);
    c.matchSystemTheme = ReadBool(kSecColor, L"MatchSystemTheme", c.matchSystemTheme, p);

    c.rotationEnabled = ReadBool(kSecRotation, L"Enabled", c.rotationEnabled, p);
    c.rotationIntervalMinutes = ReadInt(kSecRotation, L"IntervalMinutes", c.rotationIntervalMinutes, p);
    c.rotationFolder = ReadStr(kSecRotation, L"Folder", p);
    c.rotationLastSwitch = ReadInt64(kSecRotation, L"LastSwitch", c.rotationLastSwitch, p);

    c.scheduleMode = ReadInt(kSecSchedule, L"Mode", c.scheduleMode, p);
    { const std::wstring d = ReadStr(kSecSchedule, L"DayVideoPath", p); if (!d.empty()) c.dayVideoPath = d; }
    { const std::wstring n = ReadStr(kSecSchedule, L"NightVideoPath", p); if (!n.empty()) c.nightVideoPath = n; }
    c.dayStartMinutes = ReadInt(kSecSchedule, L"DayStartMinutes", c.dayStartMinutes, p);
    c.nightStartMinutes = ReadInt(kSecSchedule, L"NightStartMinutes", c.nightStartMinutes, p);
    c.latitude = ReadFloat(kSecSchedule, L"Latitude", c.latitude, p);
    c.longitude = ReadFloat(kSecSchedule, L"Longitude", c.longitude, p);

    c.hideFromCapture = ReadBool(kSecPrivacy, L"HideFromCapture", c.hideFromCapture, p);
    c.blackoutOnCapture = ReadBool(kSecPrivacy, L"BlackoutOnCapture", c.blackoutOnCapture, p);
}

bool ConfigManager::Save() {
    if (!dirty_.exchange(false)) {
        // Nothing changed since the last save — but Load() also calls Save() on
        // first boot with dirty_==false to materialize defaults, so force it if
        // the file is missing.
        const DWORD attrs = ::GetFileAttributesW(iniPath_.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
    }

    EngineConfig c = Snapshot();
    const std::wstring& p = iniPath_;

    WriteStr(kSecLibrary, L"VideoPath", c.videoPath, p);

    WriteInt(kSecDisplay, L"MonitorIndex", c.monitorIndex, p);
    WriteInt(kSecDisplay, L"Layout", static_cast<int>(c.layout), p);
    WriteInt(kSecDisplay, L"Aspect", static_cast<int>(c.aspect), p);

    WriteInt(kSecPerf, L"TargetFps", c.targetFps, p);
    WriteInt(kSecPerf, L"PauseOnMaximized", c.pauseOnMaximized ? 1 : 0, p);
    WriteInt(kSecPerf, L"PauseOnFullscreen", c.pauseOnFullscreen ? 1 : 0, p);
    WriteInt(kSecPerf, L"PauseOnFocused", c.pauseOnFocused ? 1 : 0, p);
    WriteInt(kSecPerf, L"PauseOnBattery", c.pauseOnBattery ? 1 : 0, p);

    WriteFloat(kSecAudio, L"Volume", c.volume, p);
    WriteInt(kSecAudio, L"SmartMute", c.smartMute ? 1 : 0, p);
    WriteFloat(kSecAudio, L"PlaybackSpeed", c.playbackSpeed, p);
    WriteInt(kSecAudio, L"Muted", c.muted ? 1 : 0, p);

    WriteInt(kSecSystem, L"StartWithWindows", c.startWithWindows ? 1 : 0, p);
    WriteInt(kSecSystem, L"MemoryEviction", c.memoryEviction ? 1 : 0, p);

    WriteFloat(kSecColor, L"Brightness", c.brightness, p);
    WriteFloat(kSecColor, L"Contrast", c.contrast, p);
    WriteFloat(kSecColor, L"Saturation", c.saturation, p);
    WriteFloat(kSecColor, L"Gamma", c.gamma, p);
    WriteFloat(kSecColor, L"TintR", c.tintR, p);
    WriteFloat(kSecColor, L"TintG", c.tintG, p);
    WriteFloat(kSecColor, L"TintB", c.tintB, p);
    WriteFloat(kSecColor, L"Temperature", c.temperature, p);
    WriteInt(kSecColor, L"MatchSystemTheme", c.matchSystemTheme ? 1 : 0, p);

    WriteInt(kSecRotation, L"Enabled", c.rotationEnabled ? 1 : 0, p);
    WriteInt(kSecRotation, L"IntervalMinutes", c.rotationIntervalMinutes, p);
    WriteStr(kSecRotation, L"Folder", c.rotationFolder, p);
    WriteInt64(kSecRotation, L"LastSwitch", c.rotationLastSwitch, p);

    WriteInt(kSecSchedule, L"Mode", c.scheduleMode, p);
    WriteStr(kSecSchedule, L"DayVideoPath", c.dayVideoPath, p);
    WriteStr(kSecSchedule, L"NightVideoPath", c.nightVideoPath, p);
    WriteInt(kSecSchedule, L"DayStartMinutes", c.dayStartMinutes, p);
    WriteInt(kSecSchedule, L"NightStartMinutes", c.nightStartMinutes, p);
    WriteFloat(kSecSchedule, L"Latitude", c.latitude, p);
    WriteFloat(kSecSchedule, L"Longitude", c.longitude, p);

    WriteInt(kSecPrivacy, L"HideFromCapture", c.hideFromCapture ? 1 : 0, p);
    WriteInt(kSecPrivacy, L"BlackoutOnCapture", c.blackoutOnCapture ? 1 : 0, p);

    // Flush the cache to disk.
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, p.c_str());
    return true;
}

EngineConfig ConfigManager::Snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return config_;
}

void ConfigManager::RefreshAtomics() {
    // Caller holds the unique_lock.
    aTargetFps_.store(config_.targetFps, std::memory_order_relaxed);
    aSpeed_.store(config_.playbackSpeed, std::memory_order_relaxed);
    aVolume_.store(config_.volume, std::memory_order_relaxed);
    aMuted_.store(config_.muted, std::memory_order_relaxed);
    aMemEvict_.store(config_.memoryEviction, std::memory_order_relaxed);
    aPauseMax_.store(config_.pauseOnMaximized, std::memory_order_relaxed);
    aPauseFull_.store(config_.pauseOnFullscreen, std::memory_order_relaxed);
    aPauseFocus_.store(config_.pauseOnFocused, std::memory_order_relaxed);
    aPauseBatt_.store(config_.pauseOnBattery, std::memory_order_relaxed);
    aAspect_.store(static_cast<int>(config_.aspect), std::memory_order_relaxed);
    aBrightness_.store(config_.brightness, std::memory_order_relaxed);
    aContrast_.store(config_.contrast, std::memory_order_relaxed);
    aSaturation_.store(config_.saturation, std::memory_order_relaxed);
    aGamma_.store(config_.gamma, std::memory_order_relaxed);
    aTintR_.store(config_.tintR, std::memory_order_relaxed);
    aTintG_.store(config_.tintG, std::memory_order_relaxed);
    aTintB_.store(config_.tintB, std::memory_order_relaxed);
    aTemp_.store(config_.temperature, std::memory_order_relaxed);
    aMatchTheme_.store(config_.matchSystemTheme, std::memory_order_relaxed);
    aBlackout_.store(config_.blackoutOnCapture, std::memory_order_relaxed);
}

template <typename Mutator>
void ConfigManager::Mutate(ConfigField field, Mutator&& mutate) {
    EngineConfig snapshot;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        mutate(config_);
        RefreshAtomics();
        snapshot = config_;  // capture the exact post-mutation state under lock
    }
    dirty_.store(true, std::memory_order_relaxed);
    Notify(field, snapshot);
}

void ConfigManager::Notify(ConfigField field, const EngineConfig& snapshot) {
    // Copy the observer list so callbacks can't invalidate it mid-iteration and
    // so we never hold obsMutex_ while calling out (avoids reentrancy deadlock).
    // The snapshot is the state captured atomically with the mutation, so the
    // (field, config) pair a concurrent setter delivers is always self-consistent.
    std::vector<Observer> observers;
    {
        std::scoped_lock<std::mutex> lock(obsMutex_);
        observers = observers_;
    }
    for (const Observer& obs : observers) {
        if (obs) {
            obs(snapshot, field);
        }
    }
}

// --- Typed setters ---------------------------------------------------------
void ConfigManager::SetVideoPath(std::wstring value) {
    Mutate(ConfigField::VideoPath,
           [&](EngineConfig& c) { c.videoPath = std::move(value); });
}
void ConfigManager::SetMonitorIndex(int value) {
    Mutate(ConfigField::Monitor, [&](EngineConfig& c) { c.monitorIndex = value; });
}
void ConfigManager::SetLayout(LayoutMode value) {
    Mutate(ConfigField::Layout, [&](EngineConfig& c) { c.layout = value; });
}
void ConfigManager::SetAspect(AspectMode value) {
    Mutate(ConfigField::Aspect, [&](EngineConfig& c) { c.aspect = value; });
}
void ConfigManager::SetTargetFps(int value) {
    Mutate(ConfigField::TargetFps, [&](EngineConfig& c) { c.targetFps = value; });
}
void ConfigManager::SetPauseRules(bool maximized, bool fullscreen, bool focused,
                                  bool battery) {
    Mutate(ConfigField::PauseRules, [&](EngineConfig& c) {
        c.pauseOnMaximized = maximized;
        c.pauseOnFullscreen = fullscreen;
        c.pauseOnFocused = focused;
        c.pauseOnBattery = battery;
    });
}
void ConfigManager::SetVolume(float value) {
    Mutate(ConfigField::Volume, [&](EngineConfig& c) { c.volume = value; });
}
void ConfigManager::SetMuted(bool value) {
    Mutate(ConfigField::Muted, [&](EngineConfig& c) { c.muted = value; });
}
void ConfigManager::SetSmartMute(bool value) {
    Mutate(ConfigField::SmartMute, [&](EngineConfig& c) { c.smartMute = value; });
}
void ConfigManager::SetPlaybackSpeed(float value) {
    Mutate(ConfigField::PlaybackSpeed, [&](EngineConfig& c) { c.playbackSpeed = value; });
}
void ConfigManager::SetStartWithWindows(bool value) {
    Mutate(ConfigField::StartWithWindows,
           [&](EngineConfig& c) { c.startWithWindows = value; });
}
void ConfigManager::SetMemoryEviction(bool value) {
    Mutate(ConfigField::MemoryEviction,
           [&](EngineConfig& c) { c.memoryEviction = value; });
}
void ConfigManager::SetColor(float brightness, float contrast, float saturation,
                             float gamma, float tintR, float tintG, float tintB,
                             float temperature, bool matchSystemTheme) {
    Mutate(ConfigField::Color, [&](EngineConfig& c) {
        c.brightness = brightness;
        c.contrast = contrast;
        c.saturation = saturation;
        c.gamma = gamma;
        c.tintR = tintR;
        c.tintG = tintG;
        c.tintB = tintB;
        c.temperature = temperature;
        c.matchSystemTheme = matchSystemTheme;
    });
}
void ConfigManager::SetRotation(bool enabled, int intervalMinutes,
                                std::wstring folder) {
    Mutate(ConfigField::Rotation, [&](EngineConfig& c) {
        const bool folderChanged = (c.rotationFolder != folder);
        c.rotationEnabled = enabled;
        c.rotationIntervalMinutes = intervalMinutes;
        c.rotationFolder = std::move(folder);
        // Reset the timer when the folder changes so rotation starts fresh.
        if (folderChanged) c.rotationLastSwitch = 0;
    });
}
void ConfigManager::SetRotationLastSwitch(long long epochSeconds) {
    Mutate(ConfigField::Rotation,
           [&](EngineConfig& c) { c.rotationLastSwitch = epochSeconds; });
}
void ConfigManager::SetSchedule(int mode, std::wstring dayPath,
                                std::wstring nightPath, int dayStartMinutes,
                                int nightStartMinutes, float latitude,
                                float longitude) {
    Mutate(ConfigField::Schedule, [&](EngineConfig& c) {
        c.scheduleMode = mode;
        c.dayVideoPath = std::move(dayPath);
        c.nightVideoPath = std::move(nightPath);
        c.dayStartMinutes = dayStartMinutes;
        c.nightStartMinutes = nightStartMinutes;
        c.latitude = latitude;
        c.longitude = longitude;
    });
}
void ConfigManager::SetPrivacy(bool hideFromCapture, bool blackoutOnCapture) {
    Mutate(ConfigField::Privacy, [&](EngineConfig& c) {
        c.hideFromCapture = hideFromCapture;
        c.blackoutOnCapture = blackoutOnCapture;
    });
}

void ConfigManager::AddObserver(Observer observer) {
    std::scoped_lock<std::mutex> lock(obsMutex_);
    observers_.push_back(std::move(observer));
}

}  // namespace lwe::config
