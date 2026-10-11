// SPDX-License-Identifier: MPL-2.0
//
// Mouse look, paced by when the mouse moved rather than when the window read it. The window thread
// reads motion once a window frame (the monitor's refresh), so applying it there turns the head in
// steps the 90 Hz tracking stream samples unevenly: judder. The tracking thread instead takes the
// motion whose input time is behind it by the lag motion arrives with, so each tick turns the head by
// what the mouse did in that tick's interval.

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>

namespace xrpilot
{

class MouseLook
{
public:
    // Motion the mouse made at atNs (its input timestamp), read by the window at arrivedNs; both on
    // monotonicNowNs's clock.
    void push(int64_t atNs, int64_t arrivedNs, float dx, float dy);
    // Adds to dx, dy the motion up to nowNs less the arrival lag, and drops it.
    void take(int64_t nowNs, float& dx, float& dy);
    // Drops motion not yet taken (the mouse was released).
    void clear();
    // How far behind the input the head turns now.
    int64_t delayNs() const;

    // The lag never exceeds this: motion older than it is taken at once.
    static constexpr int64_t MaxDelayNs = 50'000'000;

private:
    struct Motion
    {
        int64_t atNs;
        float dx, dy;
    };
    mutable std::mutex mutex_;
    std::deque<Motion> pending_;
    int64_t delayNs_ = 0;
    int64_t lastTakeNs_ = 0;
};

} // namespace xrpilot
