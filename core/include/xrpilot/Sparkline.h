// SPDX-License-Identifier: MPL-2.0
//
// A counter's per-tick increments over the last 40 ticks (10 s at 250 ms), ported from OXRSys
// clients/Qt/oxrsys-simulator-shared/src/SimulatorWidget.cpp.

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace xrpilot
{

class Sparkline
{
public:
    static constexpr int Samples = 40;

    enum class Colour : uint8_t
    {
        Normal,
        Fault,   // a fault series sample above zero
        Clipped, // above the scale, drawn at the top
    };

    struct Segment
    {
        float x0, y0, x1, y1;
        Colour colour;
    };

    void sample(uint64_t total);
    const std::deque<uint64_t>& deltas() const { return deltas_; }

    // Segments in a width x height box, y down. The scale is 1.5x the 90th percentile, so a spike
    // clips instead of flattening the rest; each segment takes its own sample's colour.
    std::vector<Segment> segments(float width, float height, bool faultSeries) const;

private:
    std::deque<uint64_t> deltas_;
    uint64_t last_ = 0;
    bool primed_ = false;
};

} // namespace xrpilot
