// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Agent.h"
#include "xrpilot/HumanInput.h"

#include <algorithm>
#include <cmath>

namespace xrpilot
{

namespace
{

constexpr float DegreesToRadians = 0.017453292519943295f;

void multiply(const float a[4], const float b[4], float out[4])
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void axisAngle(float x, float y, float z, float degrees, float out[4])
{
    const float half = degrees * DegreesToRadians * 0.5f;
    out[0] = x * std::sin(half);
    out[1] = y * std::sin(half);
    out[2] = z * std::sin(half);
    out[3] = std::cos(half);
}

} // namespace

AgentState::AgentState()
{
    head.position[1] = 1.6f;
}

void setSeated(AgentState& state, bool seated)
{
    if (state.seated == seated)
        return;
    state.seated = seated;
    const float dy = seated ? -SeatedEyeDrop : SeatedEyeDrop;
    state.head.position[1] += dy;
    for (HandState& hand : state.hands)
    {
        if (hand.manual)
            hand.pose.position[1] += dy;
    }
}

void poseQuaternion(const Pose& pose, float out[4])
{
    float yaw[4];
    float pitch[4];
    float roll[4];
    float yawPitch[4];
    axisAngle(0.0f, 1.0f, 0.0f, pose.yaw, yaw);
    axisAngle(1.0f, 0.0f, 0.0f, pose.pitch, pitch);
    axisAngle(0.0f, 0.0f, 1.0f, pose.roll, roll);
    multiply(yaw, pitch, yawPitch);
    multiply(yawPitch, roll, out);
}

void fillTrackingPacket(const AgentState& state, int64_t timestampNs, oxr::protocol::TrackingPacket& packet)
{
    using namespace oxr::protocol;
    packet = {};
    packet.timestampNs = timestampNs;
    std::copy(std::begin(state.head.position), std::end(state.head.position), std::begin(packet.headPosition));
    poseQuaternion(state.head, packet.headOrientation);

    const HandState& left = state.hands[0];
    const HandState& right = state.hands[1];
    const Pose leftPose = handPose(state, 0);
    const Pose rightPose = handPose(state, 1);
    std::copy(std::begin(leftPose.position), std::end(leftPose.position), std::begin(packet.leftControllerPos));
    std::copy(std::begin(rightPose.position), std::end(rightPose.position), std::begin(packet.rightControllerPos));
    poseQuaternion(leftPose, packet.leftControllerRot);
    poseQuaternion(rightPose, packet.rightControllerRot);
    packet.trackingFlags = (left.present ? TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE : 0u) |
                           (right.present ? TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE : 0u);

    packet.buttonState = state.buttons;
    packet.leftTrigger = left.trigger;
    packet.rightTrigger = right.trigger;
    packet.leftGrip = left.grip;
    packet.rightGrip = right.grip;
    if (left.trigger > 0.5f)
        packet.buttonState |= BUTTON_LEFT_TRIGGER;
    if (right.trigger > 0.5f)
        packet.buttonState |= BUTTON_RIGHT_TRIGGER;
    if (left.grip > 0.5f)
        packet.buttonState |= BUTTON_LEFT_GRIP;
    if (right.grip > 0.5f)
        packet.buttonState |= BUTTON_RIGHT_GRIP;
    std::copy(std::begin(left.stick), std::end(left.stick), std::begin(packet.leftThumbstick));
    std::copy(std::begin(right.stick), std::end(right.stick), std::begin(packet.rightThumbstick));
    addHumanKeys(state, packet);

    packet.ipd = state.ipd;
    const float halfV = std::clamp(state.verticalFovDegrees, 30.0f, 170.0f) * 0.5f * DegreesToRadians;
    const float halfH = std::atan(std::tan(halfV) * std::max(state.eyeAspect, 0.1f));
    packet.eyeFov[0] = -halfH;
    packet.eyeFov[1] = halfH;
    packet.eyeFov[2] = halfV;
    packet.eyeFov[3] = -halfV;
}

uint32_t buttonFlag(const std::string& name)
{
    using namespace oxr::protocol;
    if (name == "a")
        return BUTTON_A;
    if (name == "b")
        return BUTTON_B;
    if (name == "x")
        return BUTTON_X;
    if (name == "y")
        return BUTTON_Y;
    if (name == "menu")
        return BUTTON_MENU;
    if (name == "left_thumbstick")
        return BUTTON_LEFT_THUMBSTICK;
    if (name == "right_thumbstick")
        return BUTTON_RIGHT_THUMBSTICK;
    if (name == "headset")
        return BUTTON_HEADSET_SYSTEM;
    return 0;
}

} // namespace xrpilot
