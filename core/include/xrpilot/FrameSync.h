// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Frame sync: a frame is shown only together with the head pose the runtime rendered it from. A frame
// whose pose has not arrived waits a short while, then is refused; the last committed frame stays on
// screen with its own pose, which the compositor reprojects. A frame is never paired with another pose.

#pragma once

#include "xrpilot/FrameAssembler.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace xrpilot
{

class FrameSync final
{
public:
    explicit FrameSync(int64_t waitNs = 50'000'000) : waitNs_(waitNs) {}

    // newest is the frame just taken from the network, if any; attach joins a late pose (Client::attachRenderPose).
    // Returns a frame to show, which always carries its own render pose.
    std::optional<AssembledVideoFrame> next(std::optional<AssembledVideoFrame> newest,
                                            const std::function<bool(AssembledVideoFrame&)>& attach, int64_t nowNs)
    {
        if (newest)
        {
            if (pending_)
                ++refused_; // superseded before its pose came
            pending_ = std::move(newest);
            pendingSinceNs_ = nowNs;
        }
        if (!pending_)
            return std::nullopt;
        if (pending_->hasRenderPose || attach(*pending_))
        {
            ++committed_;
            std::optional<AssembledVideoFrame> out = std::move(pending_);
            pending_.reset();
            return out;
        }
        if (nowNs - pendingSinceNs_ > waitNs_)
        {
            ++refused_;
            pending_.reset();
        }
        return std::nullopt;
    }

    uint64_t committed() const { return committed_; }
    uint64_t refused() const { return refused_; }

private:
    int64_t waitNs_;
    std::optional<AssembledVideoFrame> pending_;
    int64_t pendingSinceNs_ = 0;
    uint64_t committed_ = 0;
    uint64_t refused_ = 0;
};

} // namespace xrpilot
