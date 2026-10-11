// SPDX-License-Identifier: MPL-2.0
//
// A counter's per-tick increments over the last 40 ticks (10 s at 250 ms), ported from OXRSys
// clients/Qt/oxrsys-simulator-shared/src/SimulatorWidget.cpp. lean/XrPilot/Spark.lean models it and
// emits core/tests/SparkModelCases.h, which core/tests/human.cpp replays.

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
        Fault,   // a fault series sample above zero, or zero on a series that expected data
        Clipped, // above the scale, drawn at the top
    };

    struct Segment
    {
        float x0, y0, x1, y1;
        Colour colour;
    };

    static constexpr int TickEvery = 4; // a tick mark once a second

    // expecting: the series should be counting now (its stream is connected), so a zero is a stall.
    void sample(uint64_t total, bool expecting = false);
    const std::deque<uint64_t>& deltas() const { return deltas_; }

    // Segments in a width x height box, y down, the base rule on its bottom edge. The scale is 1.5x the
    // 90th percentile, so a spike clips instead of flattening the rest; each segment takes its own
    // sample's colour. Zero sits one unit above the bottom, so a strip of zeros never vanishes into the rule.
    std::vector<Segment> segments(float width, float height, bool faultSeries) const;
    // The x of each sample taken on a whole second; they scroll with the samples, so even a strip of
    // zeros visibly moves.
    std::vector<float> ticks(float width) const;

private:
    std::deque<uint64_t> deltas_;
    std::deque<bool> starved_;
    uint64_t count_ = 0;
    uint64_t last_ = 0;
    bool primed_ = false;
};

} // namespace xrpilot
