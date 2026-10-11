// SPDX-License-Identifier: MPL-2.0
//
// A thread that calls tick once per period, on its own clock: whatever the window, its swapchain or the
// stream is doing, the work is done (lean/XrPilot/Spark.lean, pump_counts_time). A late tick is not
// repeated; the next one is a period after it.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace xrpilot
{

class Pump final
{
public:
    Pump(int64_t periodNs, std::function<void()> tick);
    ~Pump();

    Pump(const Pump&) = delete;
    Pump& operator=(const Pump&) = delete;

    uint64_t ticks() const;

private:
    void run();

    const int64_t periodNs_;
    std::function<void()> tick_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    uint64_t ticks_ = 0;
    std::thread thread_;
};

} // namespace xrpilot
