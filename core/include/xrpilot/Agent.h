// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// What the agent drives: head pose, two hands, buttons and analog inputs, written into the OXRSys
// tracking packet the runtime reads from any client.

#pragma once

#include <cstdint>
#include <string>

#include <oxrsys/protocol/Protocol.h>

namespace xrpilot
{

struct Pose
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    // Degrees; orientation is yaw about +Y, then pitch about +X, then roll about +Z.
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

struct HandState
{
    bool present = true;
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

    AgentState();
};

// Builds the packet the runtime reads; the left eye FOV is symmetric at verticalFovDegrees and eyeAspect.
void fillTrackingPacket(const AgentState& state, int64_t timestampNs, oxr::protocol::TrackingPacket& packet);

// Quaternion (x, y, z, w) of a pose's orientation.
void poseQuaternion(const Pose& pose, float out[4]);

// Maps a button name (trigger and grip are analog and handled apart) to its flag; 0 when unknown.
uint32_t buttonFlag(const std::string& name);

} // namespace xrpilot
