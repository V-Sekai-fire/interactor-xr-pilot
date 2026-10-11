// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Theme.h"

#include <algorithm>
#include <cmath>

namespace xrpilot
{

namespace
{

const Palette Dark{
    {22, 23, 26},    // background
    {30, 31, 35},    // panel
    {40, 42, 47},    // header
    {16, 17, 19},    // handle
    {234, 235, 238}, // text
    {166, 169, 177}, // muted
    {120, 170, 240}, // accent
    {52, 55, 61},    // control
    {66, 70, 77},    // controlHover
    {108, 112, 121}, // rule
    {0, 158, 115},   // good: bluish green
    {230, 159, 0},   // warning: orange
    {213, 94, 0},    // fault: vermillion
    {86, 180, 233},  // info: sky blue
    {240, 140, 90},  // faultText
    {245, 51, 82},   // axisX: Godot's editor gizmo red, green and blue
    {135, 214, 3},   // axisY
    {41, 140, 245},  // axisZ
};

const Palette Light{
    {243, 244, 246}, // background
    {255, 255, 255}, // panel
    {231, 233, 237}, // header
    {214, 217, 222}, // handle
    {24, 25, 28},    // text
    {84, 88, 96},    // muted
    {0, 95, 170},    // accent
    {226, 229, 234}, // control
    {210, 214, 221}, // controlHover
    {128, 133, 142}, // rule
    {0, 122, 86},    // good
    {150, 100, 0},   // warning
    {190, 70, 0},    // fault
    {0, 105, 160},   // info
    {175, 60, 0},    // faultText
    {190, 30, 55},   // axisX: the same hues, darkened for a light surface
    {60, 130, 0},    // axisY
    {20, 100, 200},  // axisZ
};

double channel(std::uint8_t v)
{
    const double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

} // namespace

const Palette& palette(Scheme scheme)
{
    return scheme == Scheme::Light ? Light : Dark;
}

Scheme schemeFor(Appearance appearance, bool systemDark)
{
    switch (appearance)
    {
    case Appearance::Light:
        return Scheme::Light;
    case Appearance::Dark:
        return Scheme::Dark;
    case Appearance::Auto:
        break;
    }
    return systemDark ? Scheme::Dark : Scheme::Light;
}

Appearance nextAppearance(Appearance appearance)
{
    return appearance == Appearance::Auto    ? Appearance::Light
           : appearance == Appearance::Light ? Appearance::Dark
                                             : Appearance::Auto;
}

const char* appearanceName(Appearance appearance)
{
    return appearance == Appearance::Auto ? "auto" : appearance == Appearance::Light ? "light" : "dark";
}

double luminance(Rgb c)
{
    return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
}

double contrast(Rgb a, Rgb b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace xrpilot
