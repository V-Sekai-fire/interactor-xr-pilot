// SPDX-License-Identifier: MPL-2.0

#include "xrpilot/Pump.h"

#include <chrono>

namespace xrpilot
{

Pump::Pump(int64_t periodNs, std::function<void()> tick)
    : periodNs_(periodNs)
    , tick_(std::move(tick))
{
    thread_ = std::thread([this] { run(); });
}

Pump::~Pump()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

uint64_t Pump::ticks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ticks_;
}

void Pump::run()
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point next = Clock::now() + std::chrono::nanoseconds(periodNs_);
    std::unique_lock<std::mutex> lock(mutex_);
    while (!wake_.wait_until(lock, next, [this] { return stopping_; }))
    {
        lock.unlock();
        tick_();
        lock.lock();
        ++ticks_;
        next += std::chrono::nanoseconds(periodNs_);
        const Clock::time_point now = Clock::now();
        if (next < now)
        {
            next = now + std::chrono::nanoseconds(periodNs_);
        }
    }
}

} // namespace xrpilot
