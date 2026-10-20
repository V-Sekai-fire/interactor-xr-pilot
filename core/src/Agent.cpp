// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Agent.h"
#include "xrpilot/HumanInput.h"
#include "xrpilot/Makima.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace xrpilot
{

namespace
{

constexpr float DegreesToRadians = 0.017453292519943295f;

// The Steam Frame controller's tip in its raw pose, from SteamVR's frame_controller render model; apps
// aim along it. Mirrored in x for the left hand.
constexpr float TipPitchDegrees = -40.0f;
constexpr float TipOrigin[3] = {-0.012694f, -0.02522f, 0.020687f};

} // namespace

Pose controllerFromAim(const Pose& aim, int hand)
{
    Pose raw;
    raw.rotation = multiply(aim.rotation, fromEuler(EulerOrder::XYZ, -TipPitchDegrees, 0.0f, 0.0f));
    const float tip[3] = {hand == 0 ? -TipOrigin[0] : TipOrigin[0], TipOrigin[1], TipOrigin[2]};
    float offset[3];
    apply(raw.rotation, tip, offset);
    for (int i = 0; i < 3; ++i)
        raw.position[i] = aim.position[i] - offset[i];
    return raw;
}


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
        {
            hand.pose.position[1] += dy;
            for (int i = 0; i < hand.pathCount; ++i)
                hand.pathPoses[i].position[1] += dy;
        }
    }
}

double agentClockS()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

void placeHand(AgentState& state, int hand, const Pose& pose, double nowS)
{
    HandState& h = state.hands[hand];
    const bool atRest = h.pathCount == 0 || nowS > h.pathTimes[h.pathCount - 1] + RestHoldS;
    if (!h.manual || atRest)
    {
        // A fresh motion from rest: the hand where it is now, held for a moment, so the curve
        // starts with no speed.
        const Pose current = handPose(state, hand);
        h.pathPoses[0] = current;
        h.pathTimes[0] = nowS - RestHoldS;
        h.pathPoses[1] = current;
        h.pathTimes[1] = nowS;
        h.pathCount = 2;
    }
    if (h.pathCount == MaxWaypoints)
    {
        for (int i = 1; i < MaxWaypoints; ++i)
        {
            h.pathPoses[i - 1] = h.pathPoses[i];
            h.pathTimes[i - 1] = h.pathTimes[i];
        }
        --h.pathCount;
    }
    const Pose& last = h.pathPoses[h.pathCount - 1];
    const double start = std::max(nowS, h.pathTimes[h.pathCount - 1]);
    float d = 0.0f;
    for (int c = 0; c < 3; ++c)
        d += (pose.position[c] - last.position[c]) * (pose.position[c] - last.position[c]);
    const double transit = std::clamp(double(std::sqrt(d) / HandSpeedMps), MinTransitS, MaxTransitS);
    h.pathPoses[h.pathCount] = pose;
    h.pathTimes[h.pathCount] = start + transit;
    ++h.pathCount;
    h.present = true;
    h.manual = true;
    advanceHands(state, nowS);
}

void advanceHands(AgentState& state, double nowS)
{
    for (HandState& h : state.hands)
    {
        if (!h.manual || h.pathCount < 2)
            continue;
        samplePath(h.pathPoses, h.pathTimes, std::size_t(h.pathCount), nowS, h.pose);
    }
}

void eyeHalfFov(const AgentState& state, float& horizontal, float& vertical)
{
    const float* t = state.renderTangents;
    if (t[1] > t[0] && t[2] > t[3])
    {
        horizontal = std::atan(std::max(-t[0], t[1]));
        vertical = std::atan(std::max(t[2], -t[3]));
        return;
    }
    vertical = std::clamp(state.verticalFovDegrees, 30.0f, 170.0f) * 0.5f * DegreesToRadians;
    horizontal = std::atan(std::tan(vertical) * std::max(state.eyeAspect, 0.1f));
}

void poseQuaternion(const Pose& pose, float out[4])
{
    toQuaternion(pose.rotation, out);
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
    const Pose leftPose = controllerFromAim(handPose(state, 0), 0);
    const Pose rightPose = controllerFromAim(handPose(state, 1), 1);
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

    std::copy(std::begin(state.headLinearVelocity), std::end(state.headLinearVelocity),
              std::begin(packet.headLinearVelocity));
    std::copy(std::begin(state.headAngularVelocity), std::end(state.headAngularVelocity),
              std::begin(packet.headAngularVelocity));

    packet.ipd = state.ipd;
    float halfH = 0.0f;
    float halfV = 0.0f;
    eyeHalfFov(state, halfH, halfV);
    packet.eyeFov[0] = -halfH;
    packet.eyeFov[1] = halfH;
    packet.eyeFov[2] = halfV;
    packet.eyeFov[3] = -halfV;
}

bool nextTrackingPacket(const AgentState& state, int64_t nowNs, int64_t& lastSampleNs,
                        oxr::protocol::TrackingPacket& packet)
{
    if (state.headSampleNs == 0)
    {
        fillTrackingPacket(state, nowNs, packet);
        return true;
    }
    // Resending a sample with a later stamp would read as a head that stopped, then jumped.
    if (state.headSampleNs <= lastSampleNs)
        return false;
    lastSampleNs = state.headSampleNs;
    fillTrackingPacket(state, state.headSampleNs, packet);
    return true;
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
