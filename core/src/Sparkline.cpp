// SPDX-License-Identifier: MPL-2.0

#include "xrpilot/Sparkline.h"

#include <algorithm>

namespace xrpilot
{

void Sparkline::sample(uint64_t total)
{
    deltas_.push_back(primed_ && total >= last_ ? total - last_ : 0);
    last_ = total;
    primed_ = true;
    if (deltas_.size() > size_t(Samples))
        deltas_.pop_front();
}

std::vector<Sparkline::Segment> Sparkline::segments(float width, float height, bool faultSeries) const
{
    std::vector<Segment> out;
    if (deltas_.size() < 2)
        return out;
    std::vector<uint64_t> sorted(deltas_.begin(), deltas_.end());
    std::sort(sorted.begin(), sorted.end());
    const uint64_t scale = std::max<uint64_t>(1, sorted[(sorted.size() - 1) * 9 / 10] * 3 / 2);
    const float step = width / float(Samples - 1);
    const float x0 = width - step * float(deltas_.size() - 1);
    std::vector<float> ys;
    for (uint64_t d : deltas_)
        ys.push_back(height - height * std::min(1.0f, float(d) / float(scale)));
    for (size_t i = 1; i < deltas_.size(); ++i)
    {
        const Colour colour = faultSeries && deltas_[i] > 0 ? Colour::Fault
                              : deltas_[i] > scale          ? Colour::Clipped
                                                            : Colour::Normal;
        out.push_back({x0 + step * float(i - 1), ys[i - 1], x0 + step * float(i), ys[i], colour});
    }
    return out;
}

} // namespace xrpilot
