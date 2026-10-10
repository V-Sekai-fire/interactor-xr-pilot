// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Pace.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <chrono>
#include <thread>

namespace xrpilot
{

#if defined(_WIN32)

namespace
{

// One high-resolution waitable timer per thread (Windows 10 1803 and later). Unlike timeBeginPeriod it
// needs no process-wide setting, and Windows 11 keeps it precise while the window is hidden.
HANDLE threadTimer()
{
    struct Timer
    {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
        ~Timer()
        {
            if (handle != nullptr)
                CloseHandle(handle);
        }
    };
    thread_local Timer timer;
    return timer.handle;
}

} // namespace

void sleepPreciseNs(int64_t ns)
{
    if (ns <= 0)
        return;
    HANDLE timer = threadTimer();
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100); // relative, in 100 ns units
    if (timer != nullptr && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
    {
        WaitForSingleObject(timer, INFINITE);
        return;
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

#else

void sleepPreciseNs(int64_t ns)
{
    if (ns > 0)
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

#endif

} // namespace xrpilot
