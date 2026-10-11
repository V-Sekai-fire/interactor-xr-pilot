// SPDX-License-Identifier: MPL-2.0
//
// HumanInput tests, ported from OXRSys clients/Qt/oxrsys-simulator-shared/tests/SimulatorSharedTests.cpp;
// one case per ctest entry.

#include "xrpilot/Commands.h"
#include "xrpilot/HeadGizmo.h"
#include "xrpilot/HumanInput.h"
#include "xrpilot/Pump.h"
#include "xrpilot/Sparkline.h"
#include "xrpilot/Theme.h"

#include "SparkModelCases.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

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

// Head z after each packet of one second at senderHz, W held, with the window at 60 Hz. onSender walks
// on the sender; otherwise the window walks and the sender sends the latest pose, as before.
std::vector<float> walkTrace(int senderHz, bool onSender)
{
    AgentState s;
    s.keys = {key::W};
    std::vector<float> z;
    const int64_t window = 1'000'000'000 / 60, sender = 1'000'000'000 / senderHz;
    int64_t nextWindow = 0;
    for (int64_t t = 0; t < 1'000'000'000; t += sender)
    {
        while (nextWindow <= t)
        {
            advanceHuman(s, 0.0f, 0.0f, onSender ? 0.0f : float(window) * 1e-9f);
            nextWindow += window;
        }
        if (onSender)
            advanceHuman(s, 0.0f, 0.0f, float(sender) * 1e-9f);
        z.push_back(s.head.position[2]);
    }
    return z;
}

int repeats(const std::vector<float>& z)
{
    int n = 0;
    for (size_t i = 1; i < z.size(); ++i)
        n += z[i] == z[i - 1];
    return n;
}

const HeadGizmo::Arc& arcOf(const HeadGizmo& g, HeadGizmo::Role role)
{
    for (const HeadGizmo::Arc& a : g.arcs)
        if (a.role == role)
            return a;
    static const HeadGizmo::Arc none{role, {}};
    return none;
}

bool near(const HeadGizmo::Point& a, float x, float y, float z) { return std::fabs(a.x - x) + std::fabs(a.y - y) + std::fabs(a.z - z) < 1e-3f; }

const std::map<std::string, std::function<void()>> cases = {
    {"human.gizmo-straight-up-and-down",
     [] {
         for (float yaw : {0.0f, 70.0f, -135.0f})
         {
             const HeadGizmo up = buildHeadGizmo(fromEuler(EulerOrder::YXZ, yaw, 90.0f, 0.0f));
             check(near(arcOf(up, HeadGizmo::Role::Pitch).points.back(), 0, 1, 0), "looking straight up ends the pitch arc at the top pole");
             check(near(arcOf(up, HeadGizmo::Role::Look).points.back(), 0, 1, 0), "and the look ray points at it");
             check(std::fabs(up.yawDegrees - yaw) < 0.5f, "the heading survives at the pole");
             const HeadGizmo down = buildHeadGizmo(fromEuler(EulerOrder::YXZ, yaw, -90.0f, 0.0f));
             check(near(arcOf(down, HeadGizmo::Role::Pitch).points.back(), 0, -1, 0), "looking straight down ends it at the bottom pole");
         }
         // Control: a level head's pitch arc stays on the horizon, nowhere near a pole.
         const HeadGizmo level = buildHeadGizmo(Rotation{});
         check(!near(arcOf(level, HeadGizmo::Role::Pitch).points.back(), 0, 1, 0), "control: a level head is not at the top pole");
         check(arcOf(level, HeadGizmo::Role::Pitch).points.back().y == 0.0f, "control: its pitch arc lies on the horizon");
     }},
    {"human.gizmo-arcs-measure-the-angles",
     [] {
         for (float yaw : {0.0f, 30.0f, -90.0f, 170.0f})
             for (float pitch : {-60.0f, -12.0f, 0.0f, 45.0f, 80.0f})
             {
                 const HeadGizmo g = buildHeadGizmo(fromEuler(EulerOrder::YXZ, yaw, pitch, 0.0f));
                 const HeadGizmo::Arc& y = arcOf(g, HeadGizmo::Role::Yaw);
                 const HeadGizmo::Arc& p = arcOf(g, HeadGizmo::Role::Pitch);
                 check(std::fabs(angleBetween(y.points.front(), y.points.back()) - std::fabs(yaw)) < 0.5f, "the yaw arc spans the yaw");
                 check(std::fabs(angleBetween(p.points.front(), p.points.back()) - std::fabs(pitch)) < 0.5f, "the pitch arc spans the pitch");
                 check((p.points.back().y > 0.0f) == (pitch > 0.0f) || pitch == 0.0f, "up is drawn up and down is drawn down");
                 check(near(p.points.back(), g.look.x, g.look.y, g.look.z), "the pitch arc ends at the look direction");
             }
         const HeadGizmo rolled = buildHeadGizmo(fromEuler(EulerOrder::YXZ, 20.0f, 10.0f, 40.0f));
         check(std::fabs(rolled.yawDegrees - 20.0f) < 0.5f && std::fabs(rolled.pitchDegrees - 10.0f) < 0.5f, "roll moves neither arc");
         for (const HeadGizmo::Segment& s : rolled.segments(100.0f))
             check(s.x0 >= 0 && s.x0 <= 100 && s.y0 >= 0 && s.y0 <= 100, "every segment stays inside its box");
     }},
    {"human.gizmo-axes-right-handed",
     [] {
         const HeadGizmo::Point x = gizmoView({1, 0, 0}), y = gizmoView({0, 1, 0}), z = gizmoView({0, 0, 1});
         const auto det = [](HeadGizmo::Point a, HeadGizmo::Point b, HeadGizmo::Point c) {
             return a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x);
         };
         check(std::fabs(det(x, y, z) - 1.0f) < 1e-4f, "the gizmo shows +X right, +Y up and +Z toward the viewer, right-handed");
         const HeadGizmo g = buildHeadGizmo(Rotation{});
         check(near(arcOf(g, HeadGizmo::Role::Look).points.back(), 0, 0, -1), "at rest the head looks along -Z");
         check(arcOf(g, HeadGizmo::Role::AxisY).points.back().y > 1.0f, "the Y axis is drawn up");
         // Control: the same axes seen in a mirror have the opposite handedness.
         const auto mirror = [](HeadGizmo::Point p) { return HeadGizmo::Point{-p.x, p.y, p.z}; };
         check(det(mirror(x), mirror(y), mirror(z)) < 0.0f, "control: a mirrored view is left-handed");
     }},
    {"human.gizmo-front-faces-ccw",
     [] {
         // A triangle wound counter-clockwise about +Z, the side facing the viewer at rest.
         const HeadGizmo::Point tri[3] = {gizmoView({0, 0, 0}), gizmoView({1, 0, 0}), gizmoView({0, 1, 0})};
         const float area = (tri[1].x - tri[0].x) * (tri[2].y - tri[0].y) - (tri[2].x - tri[0].x) * (tri[1].y - tri[0].y);
         check(area > 0.0f, "a front face reads counter-clockwise with y up (clockwise in screen pixels, y down)");
         const float flipped = (tri[2].x - tri[0].x) * (tri[1].y - tri[0].y) - (tri[1].x - tri[0].x) * (tri[2].y - tri[0].y);
         check(flipped < 0.0f, "control: the reversed triangle reads clockwise");
     }},
    {"human.theme-contrast",
     [] {
         for (Scheme scheme : {Scheme::Dark, Scheme::Light})
         {
             const Palette& p = palette(scheme);
             for (Rgb surface : {p.background, p.panel, p.header, p.control, p.controlHover})
                 check(contrast(p.text, surface) >= 4.5, "text meets WCAG AA on every surface");
             for (Rgb surface : {p.background, p.panel})
             {
                 check(contrast(p.muted, surface) >= 4.5, "muted text meets AA on panels");
                 check(contrast(p.faultText, surface) >= 4.5, "fault text meets AA on panels");
                 for (Rgb mark : {p.good, p.warning, p.fault, p.info, p.axisX, p.axisY, p.axisZ, p.accent, p.rule})
                     check(contrast(mark, surface) >= 3.0, "marks meet the 3:1 non-text ratio");
             }
         }
         check(std::fabs(contrast({0, 0, 0}, {255, 255, 255}) - 21.0) < 0.01, "black on white is 21:1");
         // Control: the old dark theme's tick colour on its panel fails the non-text ratio.
         check(contrast({65, 78, 94}, {43, 45, 48}) < 3.0, "control: a faint rule is caught");
     }},
    {"human.theme-appearance",
     [] {
         check(schemeFor(Appearance::Auto, true) == Scheme::Dark && schemeFor(Appearance::Auto, false) == Scheme::Light,
               "auto follows the system");
         check(schemeFor(Appearance::Light, true) == Scheme::Light && schemeFor(Appearance::Dark, false) == Scheme::Dark,
               "a chosen scheme wins over the system");
         Appearance a = Appearance::Auto;
         for (int i = 0; i < 3; ++i)
             a = nextAppearance(a);
         check(a == Appearance::Auto, "the toggle cycles auto, light, dark and back");
         check(nextAppearance(Appearance::Auto) != Appearance::Auto, "control: one press changes it");
     }},
    {"human.tracking-period-follows-refresh",
     [] {
         check(trackingPeriodNs(144) == 6'944'444, "144 Hz is 6.94 ms");
         check(trackingPeriodNs(0) == 11'111'111, "no announced rate is 90 Hz");
         check(trackingPeriodNs(30) == 16'666'666 && trackingPeriodNs(240) == 6'944'444, "the rate stays within 60 to 144 Hz");
     }},
    {"human.walk-on-sender-never-repeats",
     [] {
         for (int hz : {60, 72, 90, 120, 144})
         {
             const std::vector<float> z = walkTrace(hz, true);
             check(repeats(z) == 0, "walking on the sender gives every packet a new pose");
             check(std::fabs(z.back() + 2.0f) < 0.05f, "and covers 2 m in a second at any rate");
         }
         // Control: walking in a 60 Hz window repeats poses on a faster sender, so the gate can fail.
         check(repeats(walkTrace(90, false)) >= 25, "control: a 60 Hz window repeats a third of 90 Hz packets");
         check(repeats(walkTrace(144, false)) >= 75, "control: and over half of 144 Hz packets");
     }},
    {"human.wheel-sets-walk-speed",
     [] {
         AgentState s;
         const float before[3] = {s.head.position[0], s.head.position[1], s.head.position[2]};
         applyWheel(s, 1.0f);
         check(std::fabs(s.moveSpeed - 2.5f) < 1e-5f, "one notch up scales the speed by 1.25");
         check(s.head.position[0] == before[0] && s.head.position[1] == before[1] && s.head.position[2] == before[2],
               "the wheel never moves the head");
         applyWheel(s, -2.0f);
         check(std::fabs(s.moveSpeed - 1.6f) < 1e-5f, "two notches down scale it by 1/1.5625");
         applyWheel(s, 100.0f);
         check(s.moveSpeed == MaxMoveSpeed, "the speed stops at its maximum");
         applyWheel(s, -100.0f);
         check(s.moveSpeed == MinMoveSpeed, "the speed stops at its minimum");

         AgentState slow, fast;
         slow.keys = fast.keys = {key::W};
         applyWheel(fast, 4.0f);
         advanceHuman(slow, 0.0f, 0.0f, 1.0f);
         advanceHuman(fast, 0.0f, 0.0f, 1.0f);
         check(std::fabs(fast.head.position[2] / slow.head.position[2] - std::pow(WheelSpeedStep, 4.0f)) < 1e-3f,
               "walking covers distance in proportion to the set speed");
     }},
    {"human.shift-wheel-sets-hand-reach",
     [] {
         AgentState s;
         s.keys = {key::RightShift};
         const float left = s.handOffset[0][2];
         applyWheel(s, 2.0f);
         check(std::fabs(s.handOffset[1][2] - (-0.05f - 2.0f * WheelReachStep)) < 1e-5f, "Shift and the wheel push the right hand out");
         check(s.handOffset[0][2] == left, "the other hand stays");
         check(s.moveSpeed == 2.0f, "with Shift held the wheel leaves the speed alone");
         applyWheel(s, 100.0f);
         check(s.handOffset[1][2] == MinHandReach, "the reach stops at arm's length");
         applyWheel(s, -100.0f);
         check(s.handOffset[1][2] == MaxHandReach, "and at the body");
         AgentState both;
         both.keys = {key::LeftShift, key::RightShift};
         applyWheel(both, 1.0f);
         check(both.moveSpeed != 2.0f && both.handOffset[0][2] == -0.05f, "both Shifts held is no hand, so the speed changes");
     }},
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
         float yaw, pitch, roll;
         toYawPitchRoll(s.head.rotation, yaw, pitch, roll);
         check(pitch < 0.0f, "moving the mouse down looks down");
         AgentState t;
         advanceHuman(t, 100.0f, 0.0f, 0.0f);
         toYawPitchRoll(t.head.rotation, yaw, pitch, roll);
         check(yaw < 0.0f, "moving the mouse right turns right");
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
         s.head.rotation = fromEuler(EulerOrder::YXZ, 1.2f * Deg, -0.4f * Deg, 0.0f);
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

         const Pose aim = handPose(s, 1);
         const float* p = aim.position;
         const float fx = -aim.rotation.m[2], fy = -aim.rotation.m[5], fz = -aim.rotation.m[8];
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
         runCommand("hand right 0.5 1.2 -0.4 1 0 0 0 1 0 0 0 1", s, status, 0);
         const Pose placed = handPose(s, 1);
         check(placed.position[0] == 0.5f && placed.position[1] == 1.2f,
               "an agent-placed hand is sent where the agent put it");
         runCommand("release", s, status, 0);
         s.head.position[0] = 2.0f;
         check(std::abs(handPose(s, 1).position[0] - 2.22f) < 1e-4f, "release returns the hand to the body's side");
     }},
    {"agent.packet-carries-the-controller-pose-its-tip-aims-from",
     [] {
         AgentState s;
         s.hands[1].manual = true;
         s.hands[1].pose.position[0] = 0.1f;
         s.hands[1].pose.position[1] = 1.3f;
         s.hands[1].pose.position[2] = -0.4f;
         s.hands[1].pose.rotation = fromEuler(EulerOrder::YXZ, 20.0f, -10.0f, 0.0f);
         const TrackingPacket packet = packetOf(s);
         Rotation raw;
         check(fromQuaternion(packet.rightControllerRot, raw), "the packet's rotation is a quaternion");
         // SteamVR's frame_controller tip: raw pitched -40 degrees, 1-3 cm forward of the raw origin.
         const Rotation tip = multiply(raw, fromEuler(EulerOrder::XYZ, -40.0f, 0.0f, 0.0f));
         const float origin[3] = {-0.012694f, -0.02522f, 0.020687f};
         float offset[3];
         apply(raw, origin, offset);
         float worst = 0.0f;
         for (int i = 0; i < 9; ++i)
             worst = std::max(worst, std::abs(tip.m[i] - s.hands[1].pose.rotation.m[i]));
         for (int i = 0; i < 3; ++i)
             worst = std::max(worst, std::abs(packet.rightControllerPos[i] + offset[i] - s.hands[1].pose.position[i]));
         check(worst < 1e-5f, "the app's tip lands on the hand the agent aimed");
         // Control: sending the aim itself as the raw pose leaves the tip pitched 40 degrees down.
         const Rotation wrong = multiply(s.hands[1].pose.rotation, fromEuler(EulerOrder::XYZ, -40.0f, 0.0f, 0.0f));
         check(std::abs(wrong.m[5] - s.hands[1].pose.rotation.m[5]) > 0.1f, "the uncorrected tip misses");
     }},
    {"human.seated-lowers-head-and-hands",
     [] {
         AgentState s;
         s.hands[0].manual = true;
         s.hands[0].pose.position[1] = 1.0f;
         const float standingHand = handPose(s, 1).position[1];
         setSeated(s, true);
         check(std::abs(s.head.position[1] - 1.2f) < 1e-5f, "seated eyes at 1.2 m");
         check(std::abs(s.hands[0].pose.position[1] - 0.6f) < 1e-5f, "a placed hand drops with the head");
         check(std::abs(standingHand - handPose(s, 1).position[1] - SeatedEyeDrop) < 1e-5f,
               "a body-relative hand drops with the head");
         setSeated(s, true);
         check(std::abs(s.head.position[1] - 1.2f) < 1e-5f, "sitting twice does not drop twice");
         setSeated(s, false);
         check(std::abs(s.head.position[1] - 1.6f) < 1e-5f, "standing restores 1.6 m");
     }},
    {"sparkline.deltas-over-ten-seconds",
     [] {
         Sparkline s;
         for (uint64_t i = 0; i <= 50; ++i)
             s.sample(i * 3);
         check(s.deltas().size() == size_t(Sparkline::Samples), "the window keeps 40 samples");
         check(s.deltas().back() == 3 && s.deltas().front() == 3, "each sample is the increment since the last");
         Sparkline reset;
         reset.sample(100);
         reset.sample(40);
         check(reset.deltas().back() == 0, "a counter that went backwards adds nothing");
     }},
    {"sparkline.fault-colour-follows-its-sample",
     [] {
         Sparkline s;
         const uint64_t totals[] = {0, 0, 0, 2, 2, 2};
         for (uint64_t t : totals)
             s.sample(t);
         const std::vector<Sparkline::Segment> seg = s.segments(390.0f, 20.0f, true);
         check(seg.size() == 5, "one segment between each pair of samples");
         check(seg[2].colour == Sparkline::Colour::Fault, "the segment ending on the drop is red");
         check(seg[3].colour == Sparkline::Colour::Normal && seg[4].colour == Sparkline::Colour::Normal,
               "the red clears once drops stop");
         const std::vector<Sparkline::Segment> plain = s.segments(390.0f, 20.0f, false);
         check(plain[2].colour != Sparkline::Colour::Fault, "a series that is not a fault never goes red");
     }},
    {"sparkline.p90-scale-clips-a-spike",
     [] {
         Sparkline s;
         s.sample(0);
         uint64_t total = 0;
         for (int i = 0; i < 30; ++i)
             s.sample(total += 10);
         s.sample(total += 1000);
         const std::vector<Sparkline::Segment> seg = s.segments(390.0f, 30.0f, false);
         check(seg.back().colour == Sparkline::Colour::Clipped && seg.back().y1 == 0.0f, "the spike clips to the top");
         check(std::abs(seg[seg.size() - 2].y1 - 29.0f / 3.0f) < 0.01f,
               "steady samples stay at 2/3 of the span, not flattened");
     }},
    {"sparkline.model-runs",
     [] {
         for (const spark_model::Run& run : spark_model::runs)
         {
             Sparkline s;
             for (const spark_model::Op& op : run.ops)
                 s.sample(op.total, op.expecting);
             std::string colours;
             for (const Sparkline::Segment& seg : s.segments(390.0f, 20.0f, run.faultSeries))
                 colours += seg.colour == Sparkline::Colour::Fault     ? 'F'
                            : seg.colour == Sparkline::Colour::Clipped ? 'C'
                                                                       : 'N';
             if (colours != run.colours)
                 std::fprintf(stderr, "  %s: %s, the model says %s\n", run.name, colours.c_str(), run.colours);
             check(colours == run.colours, "each segment's colour is what lean/XrPilot/Spark.lean works out");
         }
     }},
    {"sparkline.zero-clears-the-base",
     [] {
         Sparkline zeros;
         for (int i = 0; i < 50; ++i)
             zeros.sample(7);
         const std::vector<Sparkline::Segment> seg = zeros.segments(390.0f, 12.0f, false);
         check(!seg.empty() && seg.back().y1 == 11.0f, "zero sits a unit above the base rule");
         check(seg.back().y1 != 12.0f, "control: not on the base rule, where it would vanish");
         check(Sparkline().segments(390.0f, 12.0f, false).empty(), "a line with no samples draws nothing");
         check(std::abs(seg.back().x1 - 390.0f) < 0.01f, "the newest sample is at the right edge");
     }},
    {"sparkline.ticks-move-every-sample",
     [] {
         Sparkline s;
         for (int i = 0; i < 50; ++i)
             s.sample(0);
         const std::vector<float> before = s.ticks(390.0f);
         s.sample(0);
         const std::vector<float> after = s.ticks(390.0f);
         check(before.size() == 10 && after.size() == 10, "one tick a second over 10 s");
         check(before != after, "the ticks move with each sample, so a strip of zeros still scrolls");
         for (int i = 0; i < Sparkline::TickEvery - 1; ++i)
             s.sample(0);
         check(s.ticks(390.0f) == before, "control: a whole second later they are back where they were");
     }},
    {"sparkline.pump-samples-without-frames",
     [] {
         // Nothing renders or calls update here: the pump alone takes the samples.
         Sparkline s;
         std::mutex m;
         uint64_t total = 0;
         {
             Pump pump(25'000'000, [&] {
                 std::lock_guard<std::mutex> lock(m);
                 s.sample(total, true);
             });
             std::this_thread::sleep_for(std::chrono::milliseconds(260));
         }
         std::lock_guard<std::mutex> lock(m);
         check(s.deltas().size() >= 8, "the pump takes a sample each period with no frame drawn");
         const std::vector<Sparkline::Segment> seg = s.segments(390.0f, 20.0f, false);
         check(!seg.empty() && seg.back().colour == Sparkline::Colour::Fault, "and the stalled stream shows red");
         const size_t stopped = s.deltas().size();
         std::this_thread::sleep_for(std::chrono::milliseconds(60));
         check(s.deltas().size() == stopped, "control: a stopped pump takes none");
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
