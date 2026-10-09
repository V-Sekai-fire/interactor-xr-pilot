// SPDX-License-Identifier: MPL-2.0
//
// A person's keyboard and mouse driving the agent state, ported from OXRSys
// clients/Qt/oxrsys-simulator-shared/src/SimulatorTracking.{h,cpp}.

#pragma once

#include "xrpilot/Agent.h"

namespace xrpilot
{

// USB HID usages, which SDL scancodes equal, so a key keeps its place on any layout. The mouse
// buttons the simulator mapped to inputs sit below zero.
namespace key
{
constexpr int A = 4, C = 6, D = 7, E = 8, F = 9, G = 10, H = 11, I = 12, J = 13, K = 14, L = 15, M = 16, N = 17;
constexpr int Q = 20, R = 21, S = 22, T = 23, W = 26, Z = 29;
constexpr int Num1 = 30, Num2 = 31, Num3 = 32, Num4 = 33;
constexpr int Right = 79, Left = 80, Down = 81, Up = 82;
constexpr int LeftShift = 225, RightShift = 229;
// The middle mouse button is the headset button; the left one is the right trigger, or the left while F is held.
constexpr int HeadsetButton = -1003;
constexpr int TriggerMouse = -1004;
} // namespace key

// Integrates mouse motion in pixels and the held keys over deltaTime seconds: look, walk, roll,
// Shift moves a hand, and the trigger, H or M raise the pointing hand.
void advanceHuman(AgentState& state, float mouseDx, float mouseDy, float deltaTime);

// Walking speed bounds, and the factor one wheel notch scales it by.
constexpr float MinMoveSpeed = 0.25f, MaxMoveSpeed = 8.0f, WheelSpeedStep = 1.25f;
// How far one wheel notch pushes a Shift-held hand along the look direction, and its reach.
constexpr float WheelReachStep = 0.05f, MinHandReach = -0.75f, MaxHandReach = 0.1f;

// The wheel, in notches (fractional for smooth wheels and trackpads; up is positive): it scales the
// walking speed, or with a Shift held pushes that hand away (up) or pulls it in (down). It never
// moves the head.
void applyWheel(AgentState& state, float notches);

// Where a hand is: the agent's pose when it placed the hand, else at the side of the body, or for the
// right hand while pointing, under the eye aiming at the gaze point.
Pose handPose(const AgentState& state, int hand);

// Adds the held keys' buttons, triggers, grips and sticks to a packet the agent state already filled.
void addHumanKeys(const AgentState& state, oxr::protocol::TrackingPacket& packet);

} // namespace xrpilot
