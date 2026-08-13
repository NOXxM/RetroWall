#pragma once
// ---------------------------------------------------------------------------
// media/FramePacer.hpp
// Locks presentation cadence to the video's own timeline using a HIGH-RESOLUTION
// waitable timer — the wait is a kernel wait, never a Sleep()/spin. Each frame's
// due time is computed from its media timestamp relative to an anchor captured
// at the first frame, so pacing tracks the source FPS with no cumulative drift.
// Reset() re-anchors after a loop/seek or after resuming from an occluded pause.
// ---------------------------------------------------------------------------
#include <windows.h>

#include "win32/Error.hpp"

namespace lwe::media {

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

class FramePacer {
public:
    FramePacer() {
        // High-resolution timer (Win10 1803+); fall back to a standard timer.
        HANDLE t = ::CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (t == nullptr) {
            t = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        timer_.reset(win32::ThrowIfInvalidHandle(t, "CreateWaitableTimerEx"));
        ::QueryPerformanceFrequency(&qpcFreq_);
    }

    // Drop the anchor; the next WaitForFrame re-establishes it and returns
    // immediately (present the first post-reset frame without delay).
    void Reset() noexcept { anchored_ = false; }

    // Block until the given media timestamp (100-ns units) is due.
    void WaitForFrame(LONGLONG mediaTime100ns) noexcept {
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);

        if (!anchored_) {
            anchored_ = true;
            anchorMediaTime_ = mediaTime100ns;
            anchorQpc_ = now.QuadPart;
            return;
        }

        // target = anchorQpc + (mediaTime - anchorMediaTime) converted to ticks
        const long double elapsed100ns =
            static_cast<long double>(mediaTime100ns - anchorMediaTime_);
        const long double ticks =
            elapsed100ns * static_cast<long double>(qpcFreq_.QuadPart) / 1.0e7L;
        const LONGLONG targetQpc = anchorQpc_ + static_cast<LONGLONG>(ticks);
        const LONGLONG deltaTicks = targetQpc - now.QuadPart;
        if (deltaTicks <= 0) {
            return;  // behind schedule: present immediately, keep the anchor
        }

        // Relative due time for SetWaitableTimer is negative 100-ns units.
        LARGE_INTEGER due{};
        due.QuadPart = -(deltaTicks * 10'000'000LL / qpcFreq_.QuadPart);
        if (::SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE)) {
            ::WaitForSingleObject(timer_.get(), 1000);
        }
    }

private:
    win32::UniqueHandle timer_;
    LARGE_INTEGER qpcFreq_{};
    bool anchored_ = false;
    LONGLONG anchorMediaTime_ = 0;
    LONGLONG anchorQpc_ = 0;
};

}  // namespace lwe::media
