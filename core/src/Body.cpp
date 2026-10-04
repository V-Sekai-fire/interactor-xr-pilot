// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Body.h"

#include <algorithm>
#include <cmath>

extern "C"
{
#include "sinew_align.h"
}

namespace xrpilot
{

namespace
{

struct V
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(V a) { return std::sqrt(dot(a, a)); }
V normalised(V a, V fallback)
{
    const float l = length(a);
    return l > 1e-6f ? a * (1.0f / l) : fallback;
}

V of(const float p[3]) { return {p[0], p[1], p[2]}; }
void store(V v, float out[3])
{
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

// Seated or standing, the arms keep their length: scale from the standing eye height.
float standingEyeHeight(const AgentState& state)
{
    return state.head.position[1] + (state.seated ? SeatedEyeDrop : 0.0f);
}

} // namespace

void shoulderPosition(const AgentState& state, int hand, const BodyModel& body, float out[3])
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    toYawPitchRoll(state.head.rotation, yaw, pitch, roll);
    const Rotation turn = fromEuler(EulerOrder::YXZ, yaw, 0.0f, 0.0f);
    const float side = hand == 0 ? -1.0f : 1.0f;
    const float k = body.scaleFor(standingEyeHeight(state));
    const float local[3] = {side * k * body.shoulderSide, -k * body.shoulderDown, k * body.shoulderBack};
    float offset[3];
    apply(turn, local, offset);
    for (int i = 0; i < 3; ++i)
        out[i] = state.head.position[i] + offset[i];
}

ArmSolve solveArm(const AgentState& state, int hand, const float target[3], const BodyModel& body)
{
    ArmSolve out;
    shoulderPosition(state, hand, body, out.shoulder);
    const V shoulder = of(out.shoulder);
    const V toTarget = of(target) - shoulder;
    out.distance = length(toTarget);
    const V forward = normalised(toTarget, V{0.0f, 0.0f, -1.0f});

    const float k = body.scaleFor(standingEyeHeight(state));
    const float a = k * body.upperArm;
    const float b = k * (body.forearm + body.palm);
    out.reachable = out.distance <= a + b;
    const float d = std::clamp(out.distance, std::abs(a - b) + 1e-3f, a + b - 1e-4f);
    const V handAt = shoulder + forward * d;

    // The elbow bends towards a pole down and out from the shoulder, in the plane the pole makes with
    // the reach, at the angle the law of cosines gives.
    const float side = hand == 0 ? -1.0f : 1.0f;
    const V pole = V{side * 0.4f, -1.0f, 0.0f};
    const V bend = normalised(pole - forward * dot(pole, forward), V{0.0f, -1.0f, 0.0f});
    const float cosA = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
    const V elbow = shoulder + forward * (a * cosA) + bend * (a * std::sqrt(1.0f - cosA * cosA));

    // The hand points along the reach with the palm level: sinew-align fits the rotation taking the
    // controller's forward and up onto those two directions.
    V up = V{0.0f, 1.0f, 0.0f};
    up = normalised(up - forward * dot(up, forward), V{0.0f, 0.0f, 1.0f});
    const double targets[6] = {forward.x, forward.y, forward.z, up.x, up.y, up.z};
    const double sources[6] = {0.0, 0.0, -1.0, 0.0, 1.0, 0.0};
    double r[9];
    sinew_align(targets, sources, 2, r);
    for (int i = 0; i < 9; ++i)
        out.hand.rotation.m[i] = float(r[i]);
    store(handAt, out.hand.position);
    store(elbow, out.elbow);
    return out;
}

} // namespace xrpilot
