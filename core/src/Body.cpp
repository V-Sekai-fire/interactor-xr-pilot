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

V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// Two bones from root towards target, bending towards pole; the end stops at their reach.
void twoBone(V root, V target, float a, float b, V pole, V& mid, V& end)
{
    const V toTarget = target - root;
    const V forward = normalised(toTarget, V{0.0f, -1.0f, 0.0f});
    const float d = std::clamp(length(toTarget), std::abs(a - b) + 1e-3f, a + b - 1e-4f);
    const V bend = normalised(pole - forward * dot(pole, forward), V{0.0f, 0.0f, -1.0f});
    const float cosA = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
    mid = root + forward * (a * cosA) + bend * (a * std::sqrt(1.0f - cosA * cosA));
    end = root + forward * d;
}

// G1 faces +Z with its left at +X; this body faces forward with its right at right.
struct G1Map
{
    const float* g1;
    V forward;
    V right;
    V up;
    V at(int joint) const { return of(g1 + 3 * joint); }
    V dir(V v) const { return forward * v.z - right * v.x + up * v.y; }
};

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

    // The elbow bends towards a pole down and out from the shoulder.
    const float side = hand == 0 ? -1.0f : 1.0f;
    V elbow;
    V handAt;
    twoBone(shoulder, of(target), a, b, V{side * 0.4f, -1.0f, 0.0f}, elbow, handAt);

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

float legScale(const AgentState& state, const LegState& legs, const BodyModel& body)
{
    const float k = body.scaleFor(standingEyeHeight(state));
    return legs.g1Leg > 1e-3f ? k * (body.thigh + body.shin) / legs.g1Leg : 1.0f;
}

SimBody composeBody(const AgentState& state, const float* g1, const float locomotion[3], LegState& legs,
                    const BodyModel& body, const ComposeOptions& options)
{
    namespace P = oxr::protocol;
    SimBody out;
    const float k = body.scaleFor(standingEyeHeight(state));
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    toYawPitchRoll(state.head.rotation, yaw, pitch, roll);
    const Rotation turn = fromEuler(EulerOrder::YXZ, yaw, 0.0f, 0.0f);
    float f[3];
    float r[3];
    const float minusZ[3] = {0.0f, 0.0f, -1.0f};
    const float plusX[3] = {1.0f, 0.0f, 0.0f};
    apply(turn, minusZ, f);
    apply(turn, plusX, r);
    const V forward = of(f);
    const V right = of(r);
    const V up = {0.0f, 1.0f, 0.0f};
    const V head = of(state.head.position);
    const V move = of(locomotion);

    const V g1Pelvis = of(g1 + 3 * g1::Pelvis);
    const G1Map map{g1, forward, right, up};
    if (!legs.started)
    {
        legs.started = true;
        legs.g1Leg = length(map.at(g1::Knee[0]) - map.at(g1::Hip[0])) + length(map.at(g1::Ankle[0]) - map.at(g1::Knee[0]));
        for (int i = 0; i < 2; ++i)
            store(map.at(g1::Ankle[i]), legs.lastG1Ankle[i]);
        store(g1Pelvis, legs.lastG1Pelvis);
    }
    const bool fresh = length(g1Pelvis - of(legs.lastG1Pelvis)) > 0.0f;
    store(g1Pelvis, legs.lastG1Pelvis);
    const float s = legScale(state, legs, body);

    V pelvis = head - up * (k * body.eyeToPelvis) - forward * (k * body.pelvisBack);
    V hips[2];
    for (int i = 0; i < 2; ++i)
        hips[i] = pelvis + right * ((i == 0 ? -1.0f : 1.0f) * k * body.hipSide) - up * (k * body.hipDown);

    // A foot is planted while G1's ankle rests near its floor.
    V targets[2];
    for (int i = 0; i < 2; ++i)
    {
        const V ankle = map.at(g1::Ankle[i]);
        const float moved = length(ankle - of(legs.lastG1Ankle[i]));
        if (fresh)
        {
            const bool resting = ankle.y < 0.06f && moved < 0.01f;
            if (resting && !legs.planted[i])
            {
                const V raw = hips[i] + map.dir(ankle - map.at(g1::Hip[i])) * s;
                store(V{raw.x, k * body.ankleHeight, raw.z} - move, legs.lock[i]);
            }
            legs.planted[i] = resting;
            store(ankle, legs.lastG1Ankle[i]);
        }
        const V knee = map.at(g1::Knee[i]);
        V raw = hips[i] + map.dir(knee - map.at(g1::Hip[i])) * s + map.dir(ankle - knee) * s;
        raw.y = std::max(raw.y, k * body.ankleHeight);
        targets[i] = options.plantFeet && legs.planted[i] ? of(legs.lock[i]) + move : raw;
    }

    // The pelvis drops until a planted leg reaches its foot.
    const float legLength = 0.999f * k * (body.thigh + body.shin);
    float drop = 0.0f;
    for (int i = 0; i < 2; ++i)
    {
        if (!(options.plantFeet && legs.planted[i]))
            continue;
        const float dx = hips[i].x - targets[i].x;
        const float dz = hips[i].z - targets[i].z;
        const float flat = dx * dx + dz * dz;
        if (flat < legLength * legLength)
            drop = std::max(drop, hips[i].y - (targets[i].y + std::sqrt(legLength * legLength - flat)));
    }
    pelvis = pelvis - up * drop;
    for (int i = 0; i < 2; ++i)
        hips[i] = hips[i] - up * drop;

    for (int i = 0; i < 2; ++i)
    {
        const V g1Knee = map.at(g1::Knee[i]);
        const V g1Mid = (map.at(g1::Hip[i]) + map.at(g1::Ankle[i])) * 0.5f;
        const V pole = map.dir(g1Knee - g1Mid) + forward * 0.05f;
        V knee;
        V ankle;
        twoBone(hips[i], targets[i], k * body.thigh, k * body.shin, pole, knee, ankle);
        V toeDir = map.dir(map.at(g1::Toe[i]) - map.at(g1::Ankle[i]));
        toeDir.y = std::min(toeDir.y, 0.0f);
        V toe = ankle + normalised(toeDir, forward) * (k * body.toe);
        if (options.plantFeet && legs.planted[i])
            toe = ankle + normalised(V{toeDir.x, 0.0f, toeDir.z}, forward) * (k * body.toe) - up * ankle.y;
        toe.y = std::max(toe.y, 0.0f);
        const int base = i == 0 ? P::BODY_LEFT_HIP : P::BODY_RIGHT_HIP;
        store(hips[i], out.joints[base]);
        store(knee, out.joints[base + 1]);
        store(ankle, out.joints[base + 2]);
        store(toe, out.joints[base + 3]);
        if (options.plantFeet && legs.planted[i])
            out.contact |= uint8_t(i == 0 ? P::BODY_CONTACT_LEFT_FOOT : P::BODY_CONTACT_RIGHT_FOOT);
    }

    store(pelvis, out.joints[P::BODY_PELVIS]);
    store(pelvis + up * (k * body.pelvisToChest), out.joints[P::BODY_CHEST]);
    store(head - up * (k * body.eyeToNeck) - forward * (k * body.neckBack), out.joints[P::BODY_NECK]);
    store(head, out.joints[P::BODY_HEAD]);
    for (int i = 0; i < 2; ++i)
    {
        float shoulder[3];
        shoulderPosition(state, i, body, shoulder);
        float target[3];
        if (state.hands[i].present)
        {
            for (int c = 0; c < 3; ++c)
                target[c] = state.hands[i].pose.position[c];
        }
        else
        {
            store(of(shoulder) - up * (k * (body.upperArm + body.forearm)) + forward * (k * 0.05f), target);
        }
        const ArmSolve arm = solveArm(state, i, target, body);
        const int base = i == 0 ? P::BODY_LEFT_SHOULDER : P::BODY_RIGHT_SHOULDER;
        store(of(arm.shoulder), out.joints[base]);
        store(of(arm.elbow), out.joints[base + 1]);
        store(of(arm.hand.position), out.joints[base + 2]);
    }
    return out;
}

} // namespace xrpilot
