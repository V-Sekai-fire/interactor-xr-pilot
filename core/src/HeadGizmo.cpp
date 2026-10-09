// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/HeadGizmo.h"

#include <algorithm>
#include <cmath>

namespace xrpilot
{

namespace
{

constexpr float Degrees = 57.29578f;
constexpr int RingSteps = 48;

// The camera: 30 degrees to the right of forward and 20 degrees above the horizon.
constexpr float CameraYaw = -30.0f / Degrees;
constexpr float CameraPitch = -20.0f / Degrees;

HeadGizmo::Point onHorizon(float yawRadians)
{
    return {-std::sin(yawRadians), 0.0f, -std::cos(yawRadians)};
}

HeadGizmo::Point onMeridian(float yawRadians, float pitchRadians)
{
    const float c = std::cos(pitchRadians);
    return {-std::sin(yawRadians) * c, std::sin(pitchRadians), -std::cos(yawRadians) * c};
}

// World to camera space: x right, y up, z toward the viewer.
HeadGizmo::Point toCamera(const HeadGizmo::Point& p)
{
    const float cy = std::cos(CameraYaw), sy = std::sin(CameraYaw);
    const float x = cy * p.x - sy * p.z;
    const float z = sy * p.x + cy * p.z;
    const float cp = std::cos(CameraPitch), sp = std::sin(CameraPitch);
    return {x, cp * p.y - sp * z, sp * p.y + cp * z};
}

} // namespace

float angleBetween(const HeadGizmo::Point& a, const HeadGizmo::Point& b)
{
    const float la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const float lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    if (la == 0.0f || lb == 0.0f)
        return 0.0f;
    const float d = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
    return std::acos(std::clamp(d, -1.0f, 1.0f)) * Degrees;
}

HeadGizmo buildHeadGizmo(const Rotation& head)
{
    HeadGizmo g;
    const float forward[3] = {0.0f, 0.0f, -1.0f};
    float look[3];
    apply(head, forward, look);
    const float len = std::sqrt(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
    g.look = {look[0] / len, look[1] / len, look[2] / len};
    const float pitch = std::asin(std::clamp(g.look.y, -1.0f, 1.0f));
    const float flat = std::sqrt(g.look.x * g.look.x + g.look.z * g.look.z);
    // At a pole the look has no heading; the head's +Y, which then points along it, supplies one.
    float yaw;
    if (flat > 1e-4f)
        yaw = std::atan2(-g.look.x, -g.look.z);
    else
    {
        const float up[3] = {0.0f, 1.0f, 0.0f};
        float headUp[3];
        apply(head, up, headUp);
        const float s = g.look.y > 0.0f ? 1.0f : -1.0f;
        yaw = std::atan2(s * headUp[0], s * headUp[2]);
    }
    g.yawDegrees = yaw * Degrees;
    g.pitchDegrees = pitch * Degrees;

    HeadGizmo::Arc outline{HeadGizmo::Role::Outline, {}};
    HeadGizmo::Arc front{HeadGizmo::Role::Horizon, {}};
    HeadGizmo::Arc back{HeadGizmo::Role::Back, {}};
    for (int i = 0; i <= RingSteps; ++i)
    {
        const float t = 2.0f * 3.14159265f * float(i) / float(RingSteps);
        outline.points.push_back({std::cos(t), std::sin(t), 0.0f}); // camera space, mapped below
        const HeadGizmo::Point p = onHorizon(t);
        (toCamera(p).z >= 0.0f ? front : back).points.push_back(p);
    }
    g.arcs.push_back(outline);
    g.arcs.push_back(back);
    g.arcs.push_back(front);
    g.arcs.push_back({HeadGizmo::Role::Pole, {{0.0f, 0.85f, 0.0f}, {0.0f, 1.0f, 0.0f}}});
    g.arcs.push_back({HeadGizmo::Role::Pole, {{0.0f, -0.85f, 0.0f}, {0.0f, -1.0f, 0.0f}}});

    const int yawSteps = std::max(1, int(std::ceil(std::fabs(g.yawDegrees) / 5.0f)));
    HeadGizmo::Arc yawArc{HeadGizmo::Role::Yaw, {}};
    for (int i = 0; i <= yawSteps; ++i)
        yawArc.points.push_back(onHorizon(yaw * float(i) / float(yawSteps)));
    g.arcs.push_back(yawArc);

    const int pitchSteps = std::max(1, int(std::ceil(std::fabs(g.pitchDegrees) / 5.0f)));
    HeadGizmo::Arc pitchArc{HeadGizmo::Role::Pitch, {}};
    for (int i = 0; i <= pitchSteps; ++i)
        pitchArc.points.push_back(onMeridian(yaw, pitch * float(i) / float(pitchSteps)));
    g.arcs.push_back(pitchArc);

    g.arcs.push_back({HeadGizmo::Role::Look, {{0.0f, 0.0f, 0.0f}, g.look}});
    return g;
}

std::vector<HeadGizmo::Segment> HeadGizmo::segments(float size) const
{
    std::vector<Segment> out;
    const float r = size * 0.42f, c = size * 0.5f;
    for (const Arc& arc : arcs)
    {
        for (size_t i = 1; i < arc.points.size(); ++i)
        {
            // The outline is already in camera space; everything else is projected.
            const Point a = arc.role == Role::Outline ? arc.points[i - 1] : toCamera(arc.points[i - 1]);
            const Point b = arc.role == Role::Outline ? arc.points[i] : toCamera(arc.points[i]);
            out.push_back({c + a.x * r, c - a.y * r, c + b.x * r, c - b.y * r, arc.role});
        }
    }
    return out;
}

} // namespace xrpilot
