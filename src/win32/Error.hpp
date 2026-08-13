#pragma once
// ---------------------------------------------------------------------------
// win32/Error.hpp
// Typed, throw-on-failure wrappers around the three Win32 return conventions
// (BOOL, HANDLE, HRESULT) plus an RAII kernel-handle. Every raw Win32 call in
// this project is funneled through one of these so no failure is swallowed.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lwe::win32 {

// Turn a Win32/HRESULT numeric code into a human-readable, UTF-8 message.
[[nodiscard]] inline std::string FormatMessageCode(DWORD code) {
    LPWSTR wide = nullptr;
    const DWORD chars = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&wide), 0, nullptr);

    std::string out;
    if (chars != 0 && wide != nullptr) {
        const int needed = ::WideCharToMultiByte(
            CP_UTF8, 0, wide, static_cast<int>(chars), nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            out.resize(static_cast<size_t>(needed));
            ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(chars),
                                  out.data(), needed, nullptr, nullptr);
        }
    }
    if (wide != nullptr) {
        ::LocalFree(wide);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

// Exception type carrying the failing code plus caller-supplied context.
class Win32Error : public std::runtime_error {
public:
    Win32Error(long code, std::string_view context)
        : std::runtime_error(BuildWhat(code, context)), code_(code) {}

    [[nodiscard]] long code() const noexcept { return code_; }

private:
    static std::string BuildWhat(long code, std::string_view context) {
        return std::format("{} [0x{:08X}: {}]", context,
                           static_cast<unsigned long>(code),
                           FormatMessageCode(static_cast<DWORD>(code)));
    }
    long code_;
};

// Convenience: throw using the current thread's last error.
[[noreturn]] inline void ThrowLastError(std::string_view context) {
    throw Win32Error(static_cast<long>(::GetLastError()), context);
}

// BOOL-returning APIs: FALSE means failure, detail in GetLastError().
inline void ThrowIfFalse(BOOL ok, std::string_view context) {
    if (!ok) {
        ThrowLastError(context);
    }
}

// Pointer-returning APIs (FindWindow, CreateWindowEx, ...): null == failure.
template <typename T>
[[nodiscard]] T* ThrowIfNull(T* ptr, std::string_view context) {
    if (ptr == nullptr) {
        ThrowLastError(context);
    }
    return ptr;
}

// HANDLE-returning APIs: null or INVALID_HANDLE_VALUE == failure.
[[nodiscard]] inline HANDLE ThrowIfInvalidHandle(HANDLE handle,
                                                 std::string_view context) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        ThrowLastError(context);
    }
    return handle;
}

// COM / DXGI / D3D APIs returning HRESULT.
inline void ThrowIfFailed(HRESULT hr, std::string_view context) {
    if (FAILED(hr)) {
        throw Win32Error(static_cast<long>(hr), context);
    }
}

// ---------------------------------------------------------------------------
// RAII wrapper for kernel handles (events, etc.). Non-copyable, movable.
// ---------------------------------------------------------------------------
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

}  // namespace lwe::win32
