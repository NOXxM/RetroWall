#pragma once
// ---------------------------------------------------------------------------
// win32/Log.hpp
// Tiny append-only diagnostic logger. Writes timestamped lines to
// %APPDATA%\NativeWallpaperEngine\log.txt (and OutputDebugString). Used to
// diagnose "nothing shows" problems on machines we can't attach a debugger to.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <cstdarg>
#include <cwchar>
#include <mutex>
#include <string>

namespace lwe::log {

inline std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

inline void Write(const wchar_t* msg) {
    std::scoped_lock<std::mutex> lock(Mutex());

    ::OutputDebugStringW(msg);
    ::OutputDebugStringW(L"\n");

    wchar_t dir[MAX_PATH] = {};
    if (::ExpandEnvironmentStringsW(L"%APPDATA%\\NativeWallpaperEngine", dir,
                                    MAX_PATH) == 0) {
        return;
    }
    ::CreateDirectoryW(dir, nullptr);
    const std::wstring path = std::wstring(dir) + L"\\log.txt";

    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t line[1200];
    ::wsprintfW(line, L"[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute,
                st.wSecond, msg);

    char utf8[2400];
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8,
                                        static_cast<int>(sizeof(utf8)), nullptr,
                                        nullptr);
    if (n > 1) {
        DWORD written = 0;
        ::WriteFile(h, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
    }
    ::CloseHandle(h);
}

inline void Writef(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    // vswprintf (not wvsprintfW) so %f / %p / %u format correctly.
    ::vswprintf(buf, 1024, fmt, args);
    va_end(args);
    Write(buf);
}

}  // namespace lwe::log
