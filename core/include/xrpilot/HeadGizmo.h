// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The head's direction as a small 3D figure: a unit sphere seen from above and to the right, with
// the horizon ring, the up and down poles, the look ray, the yaw arc along the horizon from forward
// (-Z) to where the head faces, and the pitch arc up or down from the horizon to where it looks. A
// head looking straight up draws its pitch arc to the top pole; straight down, to the bottom one.

#pragma once

#include "xrpilot/Rotation.h"

#include <vector>

namespace xrpilot
{

struct HeadGizmo
{
    enum class Role
    {
        Outline, // the sphere's silhouette
        Horizon, // the horizon ring; Back for its far half
        Back,
        Pole,    // the up and down ticks
        Yaw,     // forward to the facing direction, along the horizon
        Pitch,   // the horizon to the look direction, along its meridian
        Look,    // the centre to the look direction
        AxisX,   // the world's +X (right), +Y (up) and +Z (toward the viewer at rest; forward is -Z)
        AxisY,
        AxisZ,
    };

    struct Point
    {
        float x, y, z;
    };

    struct Arc
    {
        Role role;
        std::vector<Point> points; // on or in the unit sphere, world space
    };

    struct Segment
    {
        float x0, y0, x1, y1;
        Role role;
    };

    float yawDegrees = 0.0f;   // + turns left, as toYawPitchRoll
    float pitchDegrees = 0.0f; // + looks up, -90 to 90
    Point look{0.0f, 0.0f, -1.0f};
    std::vector<Arc> arcs;

    // Projects the arcs into a size x size box, y down, with the sphere filling most of it.
    std::vector<Segment> segments(float size) const;
};

// Built from where the head's -Z points, so it stays right at the poles where yaw is undefined.
HeadGizmo buildHeadGizmo(const Rotation& head);

// World to the gizmo camera's space: x right, y up, z toward the viewer; a rotation, so never a mirror.
HeadGizmo::Point gizmoView(const HeadGizmo::Point& world);

// The angle in degrees between two directions.
float angleBetween(const HeadGizmo::Point& a, const HeadGizmo::Point& b);

} // namespace xrpilot
