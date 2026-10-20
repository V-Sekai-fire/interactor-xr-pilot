// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Modified Akima interpolation (makima), the curve Godot's Animation tracks sample with. It passes
// through every sample, keeps the shape between them, holds flat through repeated samples and does
// not overshoot the way a straight-line extrapolation does. The hand's motion between the poses an
// agent commands is sampled from it, so a stroke of commanded hops reaches the runtime as one
// continuous motion with momentum, not a series of jumps.

#pragma once

#include "xrpilot/Agent.h"

#include <cstddef>

namespace xrpilot
{

// Six samples at times t[0] <= ... <= t[5] with values v: from = v[2] at t[2], to = v[3] at t[3],
// weight in [0, 1] between them. Coincident neighbours (equal times) take the Akima edge rule, so
// fewer than six distinct samples are fine: repeat the end sample.
double makima(const double v[6], const double t[6], double weight);

// The same curve on unit quaternions (x, y, z, w), through the log map in from's and to's frames
// as Godot's spherical_makima_interpolate_in_time does.
void sphericalMakima(const float q[6][4], const double t[6], double weight, float out[4]);

// Samples a path of n waypoints (poses at rising times) at time at: before the first waypoint it
// is the first, after the last it is the last, between two it is the makima curve through up to
// two neighbours on each side, the end waypoints repeated where the path runs out.
void samplePath(const Pose* poses, const double* times, std::size_t n, double at, Pose& out);

} // namespace xrpilot
