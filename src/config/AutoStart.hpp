#pragma once
// ---------------------------------------------------------------------------
// config/AutoStart.hpp
// Auto-start configuration under HKCU\Software\Microsoft\Windows\
// CurrentVersion\Run. Per-user (HKCU) needs no elevation.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <string>

namespace lwe::config {

// Is our Run value present?
[[nodiscard]] bool IsAutoStartEnabled() noexcept;

// Create (enable) or delete (disable) the Run value. `commandLine` is stored
// verbatim as the launch command when enabling. Returns true on success.
bool SetAutoStart(bool enable, const std::wstring& commandLine) noexcept;

}  // namespace lwe::config
