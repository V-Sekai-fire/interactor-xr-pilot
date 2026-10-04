// SPDX-License-Identifier: MPL-2.0

#include "xrpilot/HumanInput.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace xrpilot
{

namespace
{

constexpr float DegreesPerRadian = 57.29577951308232f;
constexpr float RadiansPerDegree = 0.017453292519943295f;

struct Quaternion
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Vector
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Quaternion multiply(const Quaternion& lhs, const Quaternion& rhs)
{
    return {
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    };
}

Quaternion axisAngle(float x, float y, float z, float angle)
{
    const float halfAngle = angle * 0.5f;
    const float sine = std::sin(halfAngle);
    return {x * sine, y * sine, z * sine, std::cos(halfAngle)};
}

Vector rotate(const Quaternion& q, const Vector& v)
{
    const Quaternion p = multiply(multiply(q, {v.x, v.y, v.z, 0.0f}), {-q.x, -q.y, -q.z, q.w});
    return {p.x, p.y, p.z};
}

Vector rotate(const Rotation& r, const Vector& v)
{
    const float in[3] = {v.x, v.y, v.z};
    float out[3];
    apply(r, in, out);
    return {out[0], out[1], out[2]};
}

float headYaw(const Pose& head)
{
    float yaw, pitch, roll;
    toYawPitchRoll(head.rotation, yaw, pitch, roll);
    return yaw;
}

bool held(const AgentState& state, int k)
{
    return state.keys.count(k) != 0;
}

bool heldAny(const AgentState& state, std::initializer_list<int> keys)
{
    for (int k : keys)
    {
        if (held(state, k))
        {
            return true;
        }
    }
    return false;
}

} // namespace

void advanceHuman(AgentState& state, float mouseDx, float mouseDy, float deltaTime)
{
    constexpr float MouseSensitivity = 0.003f;
    constexpr float MoveSpeed = 2.0f;

    if (heldAny(state, {key::TriggerMouse, key::H, key::M}) && !state.pointing)
    {
        state.pointing = true;
        state.pointingAge = 0.0f;
    }
    state.pointingAge += deltaTime;

    // The mouse and E/R turn the head through its yaw, pitch and roll; the matrix stays the pose.
    Pose& head = state.head;
    float yaw, pitch, roll;
    toYawPitchRoll(head.rotation, yaw, pitch, roll);
    yaw -= mouseDx * MouseSensitivity * DegreesPerRadian;
    pitch -= mouseDy * MouseSensitivity * DegreesPerRadian;
    pitch = std::clamp(pitch, -1.5f * DegreesPerRadian, 1.5f * DegreesPerRadian);
    if (held(state, key::E))
    {
        roll -= 1.5f * deltaTime * DegreesPerRadian;
    }
    if (held(state, key::R))
    {
        roll += 1.5f * deltaTime * DegreesPerRadian;
    }
    if (mouseDx != 0.0f || mouseDy != 0.0f || heldAny(state, {key::E, key::R}))
        head.rotation = fromEuler(EulerOrder::YXZ, yaw, pitch, roll);

    float forwardAmount = 0.0f;
    float strafeAmount = 0.0f;
    if (heldAny(state, {key::W, key::Z}))
    {
        forwardAmount += 1.0f;
    }
    if (held(state, key::S))
    {
        forwardAmount -= 1.0f;
    }
    if (held(state, key::D))
    {
        strafeAmount += 1.0f;
    }
    if (heldAny(state, {key::A, key::Q}))
    {
        strafeAmount -= 1.0f;
    }

    const float walkYaw = yaw * RadiansPerDegree;
    const float forwardX = -std::sin(walkYaw);
    const float forwardZ = -std::cos(walkYaw);
    const float rightX = std::cos(walkYaw);
    const float rightZ = -std::sin(walkYaw);
    const float moveX = forwardX * forwardAmount + rightX * strafeAmount;
    const float moveZ = forwardZ * forwardAmount + rightZ * strafeAmount;
    const float moveLength = std::sqrt(moveX * moveX + moveZ * moveZ);
    if (moveLength <= 0.001f)
    {
        return;
    }

    const float step = MoveSpeed * deltaTime / moveLength;
    const bool leftShift = held(state, key::LeftShift);
    const bool rightShift = held(state, key::RightShift);
    float* hand = leftShift && !rightShift ? state.handOffset[0] : rightShift && !leftShift ? state.handOffset[1] : nullptr;
    if (hand != nullptr)
    {
        hand[0] += strafeAmount * step;
        hand[2] -= forwardAmount * step;
        return;
    }
    head.position[0] += moveX * step;
    head.position[2] += moveZ * step;
}

Pose handPose(const AgentState& state, int hand)
{
    if (state.hands[hand].manual)
    {
        return state.hands[hand].pose;
    }
    const Pose& headPose = state.head;
    const Vector head = {headPose.position[0], headPose.position[1], headPose.position[2]};
    Pose out;
    if (hand == 1 && state.pointing)
    {
        // The hand points from just under and right of the eye, clear of the line of sight so the
        // avatar's hand does not cover what it clicks, at the gaze point UI-panel distance away.
        constexpr float PanelDistance = 0.6f;
        const Vector local = rotate(headPose.rotation, {0.05f, -0.10f, -0.25f});
        const Vector at = {head.x + local.x, head.y + local.y, head.z + local.z};
        const Vector g = rotate(headPose.rotation, {0.0f, 0.0f, -PanelDistance});
        const Vector aim = {head.x + g.x - at.x, head.y + g.y - at.y, head.z + g.z - at.z};
        const float length = std::max(std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z), 1e-4f);
        out.position[0] = at.x;
        out.position[1] = at.y;
        out.position[2] = at.z;
        out.rotation = fromEuler(EulerOrder::YXZ, std::atan2(-aim.x, -aim.z) * DegreesPerRadian,
                                 std::asin(std::clamp(aim.y / length, -1.0f, 1.0f)) * DegreesPerRadian, 0.0f);
        return out;
    }
    const Quaternion bodyYaw = axisAngle(0.0f, 1.0f, 0.0f, headYaw(headPose) * RadiansPerDegree);
    const float* offset = state.handOffset[hand];
    const Vector side = rotate(bodyYaw, {offset[0], offset[1], offset[2]});
    out.position[0] = head.x + side.x;
    out.position[1] = head.y + side.y;
    out.position[2] = head.z + side.z;
    out.rotation = fromEuler(EulerOrder::YXZ, headYaw(headPose), 0.0f, 0.0f);
    return out;
}

void addHumanKeys(const AgentState& state, oxr::protocol::TrackingPacket& packet)
{
    using namespace oxr::protocol;
    if (held(state, key::F))
    {
        packet.buttonState |= BUTTON_LEFT_GRIP;
        packet.leftGrip = 1.0f;
    }
    if (held(state, key::G))
    {
        packet.buttonState |= BUTTON_RIGHT_GRIP;
        packet.rightGrip = 1.0f;
    }
    if (held(state, key::HeadsetButton))
    {
        packet.buttonState |= BUTTON_HEADSET_SYSTEM;
    }
    const bool leftHand = held(state, key::F) && !held(state, key::G);
    if (held(state, key::TriggerMouse) && leftHand)
    {
        packet.buttonState |= BUTTON_LEFT_TRIGGER;
        packet.leftTrigger = 1.0f;
    }
    // The right trigger waits until the raised hand has hovered for 150 ms, so UI sees the pointer
    // arrive before the press.
    const bool rightSettled = !state.pointing || state.pointingAge >= 0.15f;
    if (held(state, key::TriggerMouse) && !leftHand && rightSettled)
    {
        packet.buttonState |= BUTTON_RIGHT_TRIGGER;
        packet.rightTrigger = 1.0f;
    }
    if (held(state, key::T))
    {
        packet.buttonState |= BUTTON_LEFT_TRIGGER;
        packet.leftTrigger = 1.0f;
    }
    if (held(state, key::H) && rightSettled)
    {
        packet.buttonState |= BUTTON_RIGHT_TRIGGER;
        packet.rightTrigger = 1.0f;
    }

    const std::pair<int, uint32_t> buttons[] = {
        {key::Num1, BUTTON_X},
        {key::Num2, BUTTON_Y},
        {key::Num3, BUTTON_A},
        {key::Num4, BUTTON_B},
        {key::M, BUTTON_MENU},
        {key::C, BUTTON_LEFT_THUMBSTICK},
        {key::N, BUTTON_RIGHT_THUMBSTICK},
    };
    for (const std::pair<int, uint32_t>& button : buttons)
    {
        if (held(state, button.first))
        {
            packet.buttonState |= button.second;
        }
    }

    const float axes[4] = {
        (held(state, key::L) ? 1.0f : 0.0f) - (held(state, key::J) ? 1.0f : 0.0f),
        (held(state, key::I) ? 1.0f : 0.0f) - (held(state, key::K) ? 1.0f : 0.0f),
        (held(state, key::Right) ? 1.0f : 0.0f) - (held(state, key::Left) ? 1.0f : 0.0f),
        (held(state, key::Up) ? 1.0f : 0.0f) - (held(state, key::Down) ? 1.0f : 0.0f),
    };
    float* sticks[4] = {&packet.leftThumbstick[0], &packet.leftThumbstick[1], &packet.rightThumbstick[0],
                        &packet.rightThumbstick[1]};
    for (int i = 0; i < 4; ++i)
    {
        if (axes[i] != 0.0f)
        {
            *sticks[i] = axes[i];
        }
    }
}

} // namespace xrpilot
