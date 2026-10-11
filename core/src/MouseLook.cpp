// SPDX-License-Identifier: MPL-2.0

#include "xrpilot/MouseLook.h"

#include <algorithm>

namespace xrpilot
{

void MouseLook::push(int64_t atNs, int64_t arrivedNs, float dx, float dy)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // The window reads motion once a frame, so the oldest motion of a read waits about a frame: the
    // head turns that far behind the mouse, so it always has the motion of the tick it is in.
    delayNs_ = std::max(delayNs_, std::clamp(arrivedNs - atNs, int64_t(0), MaxDelayNs));
    pending_.push_back({atNs, dx, dy});
}

void MouseLook::take(int64_t nowNs, float& dx, float& dy)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // The lag eases down when reads come sooner (a faster monitor, a window no longer covered), by
    // 2% of the time passed, which the next read's lag tops up again.
    if (lastTakeNs_ != 0 && nowNs > lastTakeNs_)
        delayNs_ = std::max(int64_t(0), delayNs_ - (nowNs - lastTakeNs_) / 50);
    lastTakeNs_ = nowNs;
    const int64_t cutoff = nowNs - delayNs_;
    while (!pending_.empty() && pending_.front().atNs <= cutoff)
    {
        dx += pending_.front().dx;
        dy += pending_.front().dy;
        pending_.pop_front();
    }
}

void MouseLook::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
}

int64_t MouseLook::delayNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return delayNs_;
}

} // namespace xrpilot
