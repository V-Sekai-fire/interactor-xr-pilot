// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// What the agent drives: head pose, two hands, buttons and analog inputs, written into the OXRSys
// tracking packet the runtime reads from any client.

#pragma once

#include <cstdint>
#include <set>
#include <string>

#include "xrpilot/Rotation.h"

#include <oxrsys/protocol/Protocol.h>

namespace xrpilot
{

struct Pose
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    Rotation rotation;
};

struct HandState
{
    bool present = true;
    // Set when the agent places the hand; otherwise it follows the body (HumanInput's handPose).
    bool manual = false;
    Pose pose;
    float trigger = 0.0f;
    float grip = 0.0f;
    float stick[2] = {0.0f, 0.0f};
};

struct AgentState
{
    Pose head;
    HandState hands[2]; // 0 left, 1 right
    uint32_t buttons = 0; // oxr::protocol::ButtonFlags
    float ipd = 0.064f;
    float verticalFovDegrees = 100.0f;
    float eyeAspect = 1.0f;
    // The left eye's tangents (left, right, up, down) when the runtime renders a fixed field of view;
    // all zero when it renders the one sent in eyeFov.
    float renderTangents[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // Hands rest at the sides, relative to the head in its yaw frame, so they follow walking and turning.
    float handOffset[2][3] = {{-0.22f, -0.72f, -0.05f}, {0.22f, -0.72f, -0.05f}};
    // Pointing latches with no time limit (WCAG 2.2 SC 2.2.1) until the person lowers the hand.
    bool pointing = false;
    float pointingAge = 0.0f;
    std::set<int> keys; // held keys and mouse buttons, as HumanInput's key codes
    bool seated = false;

    AgentState();
};

// The left eye's half fields of view in radians: the runtime's when it fixes them, else verticalFovDegrees and eyeAspect.
void eyeHalfFov(const AgentState& state, float& horizontal, float& vertical);

// Builds the packet the runtime reads; the left eye FOV comes from eyeHalfFov.
void fillTrackingPacket(const AgentState& state, int64_t timestampNs, oxr::protocol::TrackingPacket& packet);

// How far the eyes drop from standing to sitting on a chair: 1.6 m to 1.2 m.
constexpr float SeatedEyeDrop = 0.4f;

// Sits or stands: the head and any placed hand move by SeatedEyeDrop; body-relative hands follow the head.
void setSeated(AgentState& state, bool seated);

// Quaternion (x, y, z, w) of a pose's rotation, for the tracking packet.
void poseQuaternion(const Pose& pose, float out[4]);

// Maps a button name (trigger and grip are analog and handled apart) to its flag; 0 when unknown.
uint32_t buttonFlag(const std::string& name);

} // namespace xrpilot
