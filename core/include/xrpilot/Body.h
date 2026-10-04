// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The body the hands belong to: the ANNY rig's rest proportions (the SOMA-X route to ANNY), scaled to
// the pilot's eye height, with the shoulders hung from the head pose and each arm solved as two bones
// from shoulder to hand, so a hand reaches only as far as an arm does. The hand is turned with
// sinew-align's rotation fit. The legs follow MotionBricks-G1 (motion.elf on ggml-rd, steered by the
// stick), retargeted by direction onto the same proportions, with each foot in stance held on the floor
// where it landed in the game world.

#pragma once

#include "xrpilot/Agent.h"

namespace xrpilot
{

// Lengths in the ANNY rig's rest pose (sinew-solve core/soma_rig.h, soma_restWorld), which this body
// scales by eyeHeight / rigEyeHeight: RightArm, RightForeArm, RightHand and RightHandMiddle1 for the
// arm, the eye midpoint for the head, RightToeEnd for the floor.
struct BodyModel
{
    float rigEyeHeight = 1.826f;   // eyes above the toes
    float shoulderSide = 0.192f;   // shoulder joint out from the midline
    float shoulderDown = 0.252f;   // below the eyes
    float shoulderBack = 0.126f;   // behind the eyes
    float upperArm = 0.308f;
    float forearm = 0.292f;
    float palm = 0.037f; // wrist to the middle of the palm, where a controller is held
    float eyeToPelvis = 0.682f;
    float pelvisBack = 0.106f;
    float pelvisToChest = 0.202f;
    float eyeToNeck = 0.199f;
    float neckBack = 0.122f;
    float hipSide = 0.112f;
    float hipDown = 0.084f;
    float thigh = 0.492f;
    float shin = 0.496f;
    float ankleHeight = 0.077f; // ankle above the floor in stance
    float toe = 0.219f;

    // How much this body is scaled from the rig for a pilot whose eyes are eyeHeight above the floor.
    float scaleFor(float eyeHeight) const { return eyeHeight > 0.1f ? eyeHeight / rigEyeHeight : 1.0f; }
    float reach(float eyeHeight) const { return scaleFor(eyeHeight) * (upperArm + forearm + palm); }
};

struct ArmSolve
{
    Pose hand;            // the controller pose: at the target when it is reachable
    float shoulder[3] = {0.0f, 0.0f, 0.0f};
    float elbow[3] = {0.0f, 0.0f, 0.0f};
    bool reachable = false; // the target is within an arm's reach of the shoulder
    float distance = 0.0f;  // shoulder to target, metres
};

// MotionBricks-G1's joints the legs follow, in motion.elf's motion_live_skeleton order.
namespace g1
{
constexpr int Pelvis = 0;
constexpr int Hip[2] = {1, 8};
constexpr int Knee[2] = {4, 11};
constexpr int Ankle[2] = {6, 13};
constexpr int Toe[2] = {7, 14};
constexpr int Joints = 34;
} // namespace g1

// What the legs remember between frames: which feet are planted and where, in the game world.
struct LegState
{
    bool started = false;
    float g1Leg = 0.0f; // G1's hip-to-ankle length, from the first frame
    bool planted[2] = {false, false};
    float lock[2][3] = {};       // a planted ankle in the game world: tracking space less the locomotion
    float lastG1Ankle[2][3] = {}; // to see a foot stop moving
    float lastG1Pelvis[3] = {};   // a frame the pelvis has not moved in is a repeat, and changes nothing
};

struct SimBody
{
    float joints[oxr::protocol::BODY_JOINT_COUNT][3] = {};
    uint8_t contact = 0; // oxr::protocol::BodyContactFlags
};

struct ComposeOptions
{
    bool plantFeet = true;
};

// How far a G1 distance is in this body: the ANNY leg over G1's.
float legScale(const AgentState& state, const LegState& legs, const BodyModel& body = {});

// The whole body for one frame. g1 holds g1::Joints world positions from motion.elf; locomotion is how
// far the game world has moved past the player in tracking space, so a planted foot moves with it.
SimBody composeBody(const AgentState& state, const float* g1, const float locomotion[3], LegState& legs,
                    const BodyModel& body = {}, const ComposeOptions& options = {});

// Where a shoulder sits for the head pose: below and beside the eyes, turned with the head's yaw.
void shoulderPosition(const AgentState& state, int hand, const BodyModel& body, float out[3]);

// The arm reaching for a world point: shoulder to hand on the line to the target, at most the arm's
// reach out, the elbow bent down and outward, and the hand pointing (local -Z) along the reach with
// the palm level (local +Y towards world up).
ArmSolve solveArm(const AgentState& state, int hand, const float target[3], const BodyModel& body = {});

} // namespace xrpilot
