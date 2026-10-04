// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Rotation.h"

#include <algorithm>
#include <cmath>

namespace xrpilot
{

namespace
{

constexpr float RadiansPerDegree = 0.017453292519943295f;
constexpr float DegreesPerRadian = 57.29577951308232f;

Rotation axis(char name, float degrees)
{
    const float c = std::cos(degrees * RadiansPerDegree);
    const float s = std::sin(degrees * RadiansPerDegree);
    if (name == 'X')
        return {{1.0f, 0.0f, 0.0f, 0.0f, c, -s, 0.0f, s, c}};
    if (name == 'Y')
        return {{c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c}};
    return {{c, -s, 0.0f, s, c, 0.0f, 0.0f, 0.0f, 1.0f}};
}

const char* orderName(EulerOrder order)
{
    switch (order)
    {
        case EulerOrder::XYZ:
            return "XYZ";
        case EulerOrder::XZY:
            return "XZY";
        case EulerOrder::YXZ:
            return "YXZ";
        case EulerOrder::YZX:
            return "YZX";
        case EulerOrder::ZXY:
            return "ZXY";
        case EulerOrder::ZYX:
            return "ZYX";
    }
    return "YXZ";
}

} // namespace

bool parseEulerOrder(const std::string& text, EulerOrder& order)
{
    for (EulerOrder candidate : {EulerOrder::XYZ, EulerOrder::XZY, EulerOrder::YXZ, EulerOrder::YZX, EulerOrder::ZXY,
                                 EulerOrder::ZYX})
    {
        if (text == orderName(candidate))
        {
            order = candidate;
            return true;
        }
    }
    return false;
}

Rotation fromEuler(EulerOrder order, float aDegrees, float bDegrees, float cDegrees)
{
    const char* names = orderName(order);
    return multiply(multiply(axis(names[0], aDegrees), axis(names[1], bDegrees)), axis(names[2], cDegrees));
}

bool fromQuaternion(const float q[4], Rotation& out)
{
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 1e-6f))
        return false;
    const float x = q[0] / length;
    const float y = q[1] / length;
    const float z = q[2] / length;
    const float w = q[3] / length;
    out.m[0] = 1.0f - 2.0f * (y * y + z * z);
    out.m[1] = 2.0f * (x * y - z * w);
    out.m[2] = 2.0f * (x * z + y * w);
    out.m[3] = 2.0f * (x * y + z * w);
    out.m[4] = 1.0f - 2.0f * (x * x + z * z);
    out.m[5] = 2.0f * (y * z - x * w);
    out.m[6] = 2.0f * (x * z - y * w);
    out.m[7] = 2.0f * (y * z + x * w);
    out.m[8] = 1.0f - 2.0f * (x * x + y * y);
    return true;
}

void toQuaternion(const Rotation& r, float out[4])
{
    const float* m = r.m;
    const float trace = m[0] + m[4] + m[8];
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        out[3] = 0.25f * s;
        out[0] = (m[7] - m[5]) / s;
        out[1] = (m[2] - m[6]) / s;
        out[2] = (m[3] - m[1]) / s;
    }
    else if (m[0] > m[4] && m[0] > m[8])
    {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        out[3] = (m[7] - m[5]) / s;
        out[0] = 0.25f * s;
        out[1] = (m[1] + m[3]) / s;
        out[2] = (m[2] + m[6]) / s;
    }
    else if (m[4] > m[8])
    {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        out[3] = (m[2] - m[6]) / s;
        out[0] = (m[1] + m[3]) / s;
        out[1] = 0.25f * s;
        out[2] = (m[5] + m[7]) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        out[3] = (m[3] - m[1]) / s;
        out[0] = (m[2] + m[6]) / s;
        out[1] = (m[5] + m[7]) / s;
        out[2] = 0.25f * s;
    }
}

Rotation multiply(const Rotation& lhs, const Rotation& rhs)
{
    Rotation out;
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k)
                sum += lhs.m[row * 3 + k] * rhs.m[k * 3 + col];
            out.m[row * 3 + col] = sum;
        }
    }
    return out;
}

void apply(const Rotation& r, const float v[3], float out[3])
{
    for (int row = 0; row < 3; ++row)
        out[row] = r.m[row * 3] * v[0] + r.m[row * 3 + 1] * v[1] + r.m[row * 3 + 2] * v[2];
}

bool isRotation(const Rotation& r, float tolerance)
{
    for (float value : r.m)
    {
        if (!std::isfinite(value))
            return false;
    }
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            float dot = 0.0f;
            for (int k = 0; k < 3; ++k)
                dot += r.m[k * 3 + i] * r.m[k * 3 + j];
            if (std::abs(dot - (i == j ? 1.0f : 0.0f)) > tolerance)
                return false;
        }
    }
    const float* m = r.m;
    const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                      m[2] * (m[3] * m[7] - m[4] * m[6]);
    return std::abs(det - 1.0f) <= tolerance;
}

void toYawPitchRoll(const Rotation& r, float& yaw, float& pitch, float& roll)
{
    pitch = std::asin(std::clamp(-r.m[5], -1.0f, 1.0f)) * DegreesPerRadian;
    yaw = std::atan2(r.m[2], r.m[8]) * DegreesPerRadian;
    roll = std::atan2(r.m[3], r.m[4]) * DegreesPerRadian;
}

} // namespace xrpilot
