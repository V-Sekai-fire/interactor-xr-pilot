// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The pilot's colours by role, in a neutral light and a neutral dark scheme. Text meets WCAG AA (4.5:1) on
// the surfaces it is drawn on and marks meet the 3:1 non-text ratio; the status hues follow Okabe and Ito's
// colour-blind-safe set and are always drawn with a glyph or shape, so colour never carries meaning alone.

#pragma once

#include <cstdint>

namespace xrpilot
{

struct Rgb
{
    std::uint8_t r = 0, g = 0, b = 0;
};

enum class Scheme : std::uint8_t
{
    Dark,
    Light,
};

// What the window shows: the system's scheme, or one the user chose.
enum class Appearance : std::uint8_t
{
    Auto,
    Light,
    Dark,
};

struct Palette
{
    Rgb background, panel, header, handle, text, muted, accent, control, controlHover, rule;
    Rgb good, warning, fault, info;
    Rgb faultText; // fault as a text colour, legible at body size
    Rgb axisX, axisY, axisZ;
};

const Palette& palette(Scheme scheme);
Scheme schemeFor(Appearance appearance, bool systemDark);
Appearance nextAppearance(Appearance appearance);
const char* appearanceName(Appearance appearance);

// WCAG 2 relative luminance and contrast ratio.
double luminance(Rgb c);
double contrast(Rgb a, Rgb b);

} // namespace xrpilot
