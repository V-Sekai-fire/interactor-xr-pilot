// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Theme.h"

#include <algorithm>
#include <cmath>

namespace xrpilot
{

namespace
{

const Palette Dark{
    {11, 16, 38},    // background: night
    {18, 26, 58},    // panel: night 2
    {27, 37, 80},    // header: night 3
    {11, 16, 38},    // handle
    {239, 230, 210}, // text: chalk
    {155, 152, 152}, // muted: chalk at 62 % on night 2
    {201, 161, 74},  // accent: brass
    {27, 37, 80},    // control
    {42, 54, 112},   // controlHover: sky low
    {155, 152, 152}, // rule
    {95, 194, 182},  // good
    {240, 180, 90},  // warning
    {224, 122, 122}, // fault
    {165, 139, 224}, // info: note
    {224, 122, 122}, // faultText
    {245, 51, 82},   // axisX: Godot's editor gizmo red, green and blue
    {135, 214, 3},   // axisY
    {41, 140, 245},  // axisZ
};

const Palette Light{
    {238, 238, 235}, // background: paper
    {251, 251, 249}, // panel: card
    {228, 228, 224}, // header: paper 2
    {200, 200, 194}, // handle: line
    {34, 34, 34},    // text: ink
    {107, 107, 107}, // muted
    {40, 120, 217},  // accent: lit
    {228, 228, 224}, // control
    {200, 200, 194}, // controlHover
    {107, 107, 107}, // rule: the design's line is 1.6 : 1, under the 3 : 1 a mark needs
    {11, 121, 112},  // good
    {141, 94, 1},    // warning
    {172, 76, 78},   // fault
    {119, 93, 173},  // info: note
    {172, 76, 78},   // faultText
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
