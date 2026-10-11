// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace oxrsys::eye_fov
{

// The left eye's field of view as a client sends it (TrackingPacket::eyeFov): the angles in radians to the
// left, right, up and down edges, left and down negative. The right eye mirrors it.
struct Angles
{
    float left = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float down = 0.0f;
};

// The tangents of those angles, with the same signs: what StreamingServer::SetRenderEyes and the announce's
// renderEyeTangents carry.
struct Tangents
{
    float left = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float down = 0.0f;
};

// The 90 degree square view the driver renders until a headset reports its own.
constexpr float kQuarterTurn = 0.785398163f;
constexpr Angles kDefault = {-kQuarterTurn, kQuarterTurn, kQuarterTurn, -kQuarterTurn};

// A field of view a renderer can use: each edge on its own side of the centre and short of 85 degrees.
inline bool Valid(const Angles& a)
{
    constexpr float kMax = 1.4835f; // 85 degrees
    return a.left < 0.0f && a.right > 0.0f && a.up > 0.0f && a.down < 0.0f && -a.left < kMax && a.right < kMax &&
           a.up < kMax && -a.down < kMax;
}

inline Angles FromPacket(const float eyeFov[4])
{
    return {eyeFov[0], eyeFov[1], eyeFov[2], eyeFov[3]};
}

inline Tangents ToTangents(const Angles& a)
{
    return {std::tan(a.left), std::tan(a.right), std::tan(a.up), std::tan(a.down)};
}

// The right eye: its left edge is the left eye's right edge, mirrored.
inline Tangents Mirror(const Tangents& t)
{
    return {-t.right, -t.left, t.up, t.down};
}

// IVRDisplayComponent::GetProjectionRaw: left and right as tangents, top and bottom with y pointing down.
struct ProjectionRaw
{
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
};

inline ProjectionRaw ToProjectionRaw(const Tangents& t)
{
    return {t.left, t.right, -t.up, -t.down};
}

// The eye width that keeps pixels square for this view at the given height, rounded to an even number.
inline uint32_t EyeWidth(const Tangents& t, uint32_t height)
{
    const float width = static_cast<float>(height) * (t.right - t.left) / (t.up - t.down);
    const uint32_t rounded = static_cast<uint32_t>(std::lround(width));
    return rounded + (rounded & 1u);
}

// Whether two views differ by more than the tolerance at any edge, in radians.
inline bool Differs(const Angles& a, const Angles& b, float tolerance = 0.01f)
{
    return std::fabs(a.left - b.left) > tolerance || std::fabs(a.right - b.right) > tolerance ||
           std::fabs(a.up - b.up) > tolerance || std::fabs(a.down - b.down) > tolerance;
}

// The eye heights a fixed-size renderer accepts.
constexpr uint32_t kMinEyeHeight = 512;
constexpr uint32_t kMaxEyeHeight = 4096;

inline bool ValidHeight(uint32_t height)
{
    return height >= kMinEyeHeight && height <= kMaxEyeHeight;
}

// The eye height to save for the next start when a headset reports its own: the headset's when it is one a
// renderer accepts and differs from both the height rendered now and the one already saved, else zero.
inline uint32_t HeightToSave(uint32_t reported, uint32_t rendering, uint32_t saved)
{
    return ValidHeight(reported) && reported != rendering && reported != saved ? reported : 0u;
}

// The settings text: "left,right,up,down" in radians.
inline std::string Format(const Angles& a)
{
    char text[96];
    std::snprintf(text, sizeof(text), "%.4f,%.4f,%.4f,%.4f", a.left, a.right, a.up, a.down);
    return text;
}

inline bool Parse(const char* text, Angles* out)
{
    Angles a;
    if (text == nullptr || std::sscanf(text, "%f,%f,%f,%f", &a.left, &a.right, &a.up, &a.down) != 4 || !Valid(a))
    {
        return false;
    }
    *out = a;
    return true;
}

} // namespace oxrsys::eye_fov
