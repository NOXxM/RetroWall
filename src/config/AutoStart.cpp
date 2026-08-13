#include "config/AutoStart.hpp"

namespace lwe::config {

namespace {
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"LiveWallpaperEngine";
}  // namespace

bool IsAutoStartEnabled() noexcept {
    // Presence check only; RRF_RT_REG_SZ restricts to string values.
    DWORD type = 0;
    DWORD bytes = 0;
    const LSTATUS status =
        ::RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ,
                       &type, nullptr, &bytes);
    return status == ERROR_SUCCESS;
}

bool SetAutoStart(bool enable, const std::wstring& commandLine) noexcept {
    if (enable) {
        const DWORD bytes =
            static_cast<DWORD>((commandLine.size() + 1) * sizeof(wchar_t));
        const LSTATUS status = ::RegSetKeyValueW(
            HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ,
            commandLine.c_str(), bytes);
        return status == ERROR_SUCCESS;
    }

    const LSTATUS status =
        ::RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
    // Deleting a value that isn't there is still "disabled" — treat as success.
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

}  // namespace lwe::config
