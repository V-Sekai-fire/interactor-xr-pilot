// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// A rotation is a row-major 3x3 matrix taking pose-local vectors to world space; its columns are the
// local +X, +Y and +Z axes in world space. Euler angles and quaternions are inputs converted to it.

#pragma once

#include <string>

namespace xrpilot
{

struct Rotation
{
    float m[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

// The six Tait-Bryan orders, each intrinsic: YXZ with angles (a, b, c) is Ry(a) * Rx(b) * Rz(c).
enum class EulerOrder
{
    XYZ,
    XZY,
    YXZ,
    YZX,
    ZXY,
    ZYX,
};

bool parseEulerOrder(const std::string& text, EulerOrder& order);
Rotation fromEuler(EulerOrder order, float aDegrees, float bDegrees, float cDegrees);
// Quaternion x, y, z, w; normalised first, and false for one of zero length.
bool fromQuaternion(const float q[4], Rotation& out);
void toQuaternion(const Rotation& r, float out[4]);

Rotation multiply(const Rotation& lhs, const Rotation& rhs);
void apply(const Rotation& r, const float v[3], float out[3]);

// Orthonormal with determinant +1 within tolerance, so scales, shears and mirrors are refused.
bool isRotation(const Rotation& r, float tolerance = 1e-3f);

// Yaw about +Y, then pitch about +X, then roll about +Z, in degrees: the YXZ angles of r. They feed
// the mouse look and the walk direction; the matrix stays the pose.
void toYawPitchRoll(const Rotation& r, float& yaw, float& pitch, float& roll);

} // namespace xrpilot
