// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The body the hands belong to: the ANNY rig's rest proportions (the SOMA-X route to ANNY), scaled to
// the pilot's eye height, with the shoulders hung from the head pose and each arm solved as two bones
// from shoulder to hand, so a hand reaches only as far as an arm does. The hand is turned with
// sinew-align's rotation fit.

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

// Where a shoulder sits for the head pose: below and beside the eyes, turned with the head's yaw.
void shoulderPosition(const AgentState& state, int hand, const BodyModel& body, float out[3]);

// The arm reaching for a world point: shoulder to hand on the line to the target, at most the arm's
// reach out, the elbow bent down and outward, and the hand pointing (local -Z) along the reach with
// the palm level (local +Y towards world up).
ArmSolve solveArm(const AgentState& state, int hand, const float target[3], const BodyModel& body = {});

} // namespace xrpilot
