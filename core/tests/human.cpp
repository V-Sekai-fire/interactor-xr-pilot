// SPDX-License-Identifier: MPL-2.0
//
// HumanInput tests, ported from OXRSys clients/Qt/oxrsys-simulator-shared/tests/SimulatorSharedTests.cpp;
// one case per ctest entry.

#include "xrpilot/Commands.h"
#include "xrpilot/HumanInput.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>

using namespace xrpilot;
using namespace oxr::protocol;

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

TrackingPacket packetOf(const AgentState& s)
{
    TrackingPacket p;
    fillTrackingPacket(s, 42, p);
    return p;
}

const std::map<std::string, std::function<void()>> cases = {
    {"human.walk-moves-head-shift-moves-hand",
     [] {
         AgentState walked;
         walked.keys = {key::W};
         advanceHuman(walked, 0.0f, 0.0f, 1.0f);
         check(walked.head.position[2] < -1.9f, "W walks the head forward");
         check(walked.handOffset[0][2] == -0.05f, "walking leaves the left hand's offset alone");

         AgentState left;
         left.keys = {key::W, key::LeftShift};
         advanceHuman(left, 0.0f, 0.0f, 1.0f);
         check(left.head.position[2] == 0.0f, "left Shift keeps the head in place");
         check(left.handOffset[0][2] < -1.9f, "left Shift moves the left hand");

         AgentState right;
         right.keys = {key::W, key::RightShift};
         advanceHuman(right, 0.0f, 0.0f, 1.0f);
         check(right.head.position[2] == 0.0f, "right Shift keeps the head in place");
         check(right.handOffset[1][2] < -1.9f, "right Shift moves the right hand");

         const TrackingPacket p = packetOf(left);
         check((p.trackingFlags & TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE) != 0 &&
                   (p.trackingFlags & TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE) != 0,
               "both controllers are active");
         check(p.timestampNs == 42, "the caller's timestamp is kept");
         check(p.eyeFov[0] < 0.0f && p.eyeFov[1] > 0.0f && p.eyeFov[2] > 0.0f && p.eyeFov[3] < 0.0f,
               "the packet carries the eye FOV");
     }},
    {"human.mouse-look-not-inverted",
     [] {
         AgentState s;
         advanceHuman(s, 0.0f, 100.0f, 0.0f);
         check(s.head.pitch < 0.0f, "moving the mouse down looks down");
         AgentState t;
         advanceHuman(t, 100.0f, 0.0f, 0.0f);
         check(t.head.yaw < 0.0f, "moving the mouse right turns right");
     }},
    {"human.touch-keys",
     [] {
         AgentState idle;
         const TrackingPacket none = packetOf(idle);
         check(none.buttonState == 0 && none.leftTrigger == 0.0f && none.rightThumbstick[0] == 0.0f,
               "no keys, no input");

         AgentState s;
         s.keys = {key::T, key::H, key::Num1, key::Num2, key::Num3, key::Num4, key::M,
                   key::C, key::N, key::I, key::L, key::Left, key::Down};
         const TrackingPacket p = packetOf(s);
         const uint32_t expected = BUTTON_LEFT_TRIGGER | BUTTON_RIGHT_TRIGGER | BUTTON_X | BUTTON_Y | BUTTON_A |
                                   BUTTON_B | BUTTON_MENU | BUTTON_LEFT_THUMBSTICK | BUTTON_RIGHT_THUMBSTICK;
         check(p.buttonState == expected, "every Touch button key sets its bit");
         check(p.leftTrigger == 1.0f && p.rightTrigger == 1.0f, "T and H pull the triggers");
         check(p.leftThumbstick[0] == 1.0f && p.leftThumbstick[1] == 1.0f, "I and L push the left stick up and right");
         check(p.rightThumbstick[0] == -1.0f && p.rightThumbstick[1] == -1.0f,
               "Left and Down push the right stick left and down");
     }},
    {"human.controllers-flag",
     [] {
         const uint32_t both = TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE | TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
         AgentState s;
         s.keys = {key::H};
         check((packetOf(s).trackingFlags & both) == both, "both controllers active by default");
         s.hands[0].present = false;
         s.hands[1].present = false;
         const TrackingPacket absent = packetOf(s);
         check((absent.trackingFlags & both) == 0, "no controller active with controllers off");
         check(absent.rightTrigger == 1.0f, "input is kept with controllers off");
     }},
    {"human.hands-aim-at-gaze",
     [] {
         constexpr float Deg = 57.29577951308232f;
         AgentState s;
         s.head.yaw = 1.2f * Deg;
         s.head.pitch = -0.4f * Deg;
         s.head.position[0] = 3.0f;
         const TrackingPacket idle = packetOf(s);
         check(idle.rightControllerPos[1] < 1.0f && idle.leftControllerPos[1] < 1.0f,
               "both hands at the sides while not pointing");

         s.keys = {key::TriggerMouse};
         advanceHuman(s, 0.0f, 0.0f, 0.011f);
         check(packetOf(s).rightTrigger == 0.0f, "the trigger waits while the hand rises");
         for (int i = 0; i < 20; ++i)
             advanceHuman(s, 0.0f, 0.0f, 0.011f);
         const TrackingPacket packet = packetOf(s);
         check(packet.rightTrigger == 1.0f, "the trigger pulls once the hand has hovered");

         const float* p = packet.rightControllerPos;
         const float* q = packet.rightControllerRot;
         const float fx = -2.0f * (q[0] * q[2] + q[3] * q[1]);
         const float fy = -2.0f * (q[1] * q[2] - q[3] * q[0]);
         const float fz = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
         const float ux = -std::sin(1.2f) * std::cos(-0.4f), uy = std::sin(-0.4f), uz = -std::cos(1.2f) * std::cos(-0.4f);
         const float hx = p[0] - 3.0f, hy = p[1] - 1.6f, hz = p[2];
         const float onLine = hx * ux + hy * uy + hz * uz;
         const float offLine = std::sqrt(std::max(0.0f, hx * hx + hy * hy + hz * hz - onLine * onLine));
         check(offLine > 0.08f, "the raised hand is clear of the line of sight");
         const float gx = 3.0f + 0.6f * ux, gy = 1.6f + 0.6f * uy, gz = 0.6f * uz;
         const float dx = gx - p[0], dy = gy - p[1], dz = gz - p[2];
         const float along = dx * fx + dy * fy + dz * fz;
         const float miss = std::sqrt(std::max(0.0f, dx * dx + dy * dy + dz * dz - along * along));
         check(along > 0.0f && miss < 0.001f, "the hand's laser passes within 1 mm of the gaze point at 0.6 m");

         s.keys.clear();
         advanceHuman(s, 0.0f, 0.0f, 60.0f);
         check(packetOf(s).rightControllerPos[1] > 1.0f, "pointing stays up with no time limit");
         s.pointing = false;
         check(packetOf(s).rightControllerPos[1] < 1.0f, "the right hand is back at the side once lowered");
     }},
    {"human.agent-hand-overrides-body",
     [] {
         AgentState s;
         ClientStatus status;
         runCommand("hand right 0.5 1.2 -0.4 10 -5", s, status, 0);
         const TrackingPacket placed = packetOf(s);
         check(placed.rightControllerPos[0] == 0.5f && placed.rightControllerPos[1] == 1.2f,
               "an agent-placed hand is sent where the agent put it");
         runCommand("release", s, status, 0);
         s.head.position[0] = 2.0f;
         const TrackingPacket released = packetOf(s);
         check(std::abs(released.rightControllerPos[0] - 2.22f) < 1e-4f, "release returns the hand to the body's side");
     }},
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || cases.count(argv[1]) == 0)
    {
        std::fprintf(stderr, "usage: xrpilot-human-tests <case>\n");
        return 2;
    }
    cases.at(argv[1])();
    return failures == 0 ? 0 : 1;
}
