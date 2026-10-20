// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Closed intervals with outward rounding, so a test states the bound a computed value must land
// in from the arithmetic it went through, instead of a tolerance picked by hand. A value copied
// from an input is compared exactly; a value computed over n roundings is contained in the exact
// result widened by n units in the last place of the type it was computed in.

#pragma once

#include <cmath>
#include <limits>

namespace xrpilot
{

struct Interval
{
    double lo = 0.0;
    double hi = 0.0;

    static Interval exact(double v) { return {v, v}; }

    bool contains(double v) const { return lo <= v && v <= hi; }

    // Widened outward by n float32 or float64 units in the last place of the larger magnitude end.
    Interval ulps32(int n) const
    {
        Interval r = *this;
        for (int i = 0; i < n; ++i)
        {
            r.lo = double(std::nextafter(float(r.lo), -std::numeric_limits<float>::infinity()));
            r.hi = double(std::nextafter(float(r.hi), std::numeric_limits<float>::infinity()));
        }
        return r;
    }
    Interval ulps64(int n) const
    {
        Interval r = *this;
        for (int i = 0; i < n; ++i)
        {
            r.lo = std::nextafter(r.lo, -std::numeric_limits<double>::infinity());
            r.hi = std::nextafter(r.hi, std::numeric_limits<double>::infinity());
        }
        return r;
    }

    Interval operator+(const Interval& o) const { return Interval{lo + o.lo, hi + o.hi}.ulps64(1); }
    Interval operator-(const Interval& o) const { return Interval{lo - o.hi, hi - o.lo}.ulps64(1); }
    Interval operator*(const Interval& o) const
    {
        const double a = lo * o.lo, b = lo * o.hi, c = hi * o.lo, d = hi * o.hi;
        return Interval{std::fmin(std::fmin(a, b), std::fmin(c, d)), std::fmax(std::fmax(a, b), std::fmax(c, d))}.ulps64(1);
    }
};

// sin and cos of a point, as intervals one double rounding wide (the libm result is within 1 ulp).
inline Interval sinI(double x)
{
    return Interval::exact(std::sin(x)).ulps64(1);
}
inline Interval cosI(double x)
{
    return Interval::exact(std::cos(x)).ulps64(1);
}

} // namespace xrpilot
