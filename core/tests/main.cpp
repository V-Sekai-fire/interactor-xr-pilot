// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Core tests, one case per ctest entry. Each negative case asserts that broken input is rejected.

#include "xrpilot/Agent.h"
#include "xrpilot/Client.h"
#include "xrpilot/Launch.h"
#include "xrpilot/McpLink.h"
#include "xrpilot/MouseLook.h"
#include "xrpilot/Pace.h"
#include "xrpilot/Body.h"
#include "xrpilot/Commands.h"
#include "xrpilot/FrameAssembler.h"
#include "xrpilot/Json.h"
#include "xrpilot/Png.h"
#include "xrpilot/Rotation.h"
#include "xrpilot/SpanLog.h"
#include "xrpilot/TraceStore.h"
#include "xrpilot/UiTrace.h"

#include <sqlite3.h>

#include <oxrsys/protocol/FecCodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#undef far
#undef near
#else
#include <sys/stat.h>
#endif

using namespace xrpilot;

namespace
{

int failures = 0;

// A UDP port the system reports free: bind port 0, read what it got, let it go.
uint16_t freeUdpPort()
{
    std::string error;
    const intptr_t s = xrpilot::bindExclusiveUdp(0, &error);
    const uint16_t port = xrpilot::boundUdpPort(s);
    xrpilot::closeUdp(s);
    return port;
}

int64_t countRows(sqlite3* db, const char* sql)
{
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
    const int64_t n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : -1;
    sqlite3_finalize(s);
    return n;
}

// Whether only the current user can open the file: a protected DACL whose one entry is this user, or
// no group or other bits.
bool userOnly(const std::string& path)
{
#if defined(_WIN32)
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoA(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                              nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(descriptor, &control, &revision);
    ACL_SIZE_INFORMATION size{};
    const bool one = dacl != nullptr && GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) &&
                     size.AceCount == 1;
    LocalFree(descriptor);
    return one && (control & SE_DACL_PROTECTED) != 0;
#else
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && (st.st_mode & 077) == 0;
#endif
}

float distance3(const float* p, const float* q)
{
    return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
}

// An analytic G1 walk at 0.8 m/s along +Z, one stride a second, the pelvis swaying 3 cm and bobbing 2 cm:
// each foot rests at its footprint for 60% of the cycle and swings to the next; legs 0.4 + 0.4 m.
void syntheticG1(float t, float out[g1::Joints * 3], bool& stance0, bool& stance1)
{
    const float speed = 0.8f;
    const float period = 1.0f;
    const float pi = 3.14159265f;
    for (int i = 0; i < g1::Joints * 3; ++i)
        out[i] = 0.0f;
    const float pelvis[3] = {0.03f * std::sin(2.0f * pi * t / period), 0.8f + 0.02f * std::cos(4.0f * pi * t / period),
                             speed * t};
    for (int c = 0; c < 3; ++c)
        out[3 * g1::Pelvis + c] = pelvis[c];
    for (int i = 0; i < 2; ++i)
    {
        const float cycle = t / period + 0.5f * float(i);
        const float n = std::floor(cycle);
        const float phase = cycle - n;
        const float footprint = speed * period * (n - 0.5f * float(i)) + speed * period * 0.3f;
        float ankle[3] = {i == 0 ? 0.1f : -0.1f, 0.04f, footprint};
        if (phase >= 0.6f)
        {
            const float u = (phase - 0.6f) / 0.4f;
            ankle[2] = footprint + speed * period * u;
            ankle[1] = 0.04f + 0.15f * std::sin(pi * u);
        }
        (i == 0 ? stance0 : stance1) = phase < 0.6f;
        const float hip[3] = {pelvis[0] + (i == 0 ? 0.1f : -0.1f), pelvis[1] - 0.05f, pelvis[2]};
        float mid[3];
        float span[3];
        for (int c = 0; c < 3; ++c)
        {
            mid[c] = 0.5f * (hip[c] + ankle[c]);
            span[c] = ankle[c] - hip[c];
        }
        const float d = std::sqrt(span[0] * span[0] + span[1] * span[1] + span[2] * span[2]);
        const float bend = std::sqrt(std::max(0.0f, 0.16f - 0.25f * d * d));
        // Forward (+Z) and perpendicular to the leg in its sagittal plane.
        const float perpY = span[2] / std::sqrt(span[1] * span[1] + span[2] * span[2]);
        const float perpZ = -span[1] / std::sqrt(span[1] * span[1] + span[2] * span[2]);
        const float knee[3] = {mid[0], mid[1] + bend * perpY, mid[2] + bend * perpZ};
        for (int c = 0; c < 3; ++c)
        {
            out[3 * g1::Hip[i] + c] = hip[c];
            out[3 * g1::Knee[i] + c] = knee[c];
            out[3 * g1::Ankle[i] + c] = ankle[c];
        }
        out[3 * g1::Toe[i]] = ankle[0];
        out[3 * g1::Toe[i] + 1] = ankle[1] - 0.03f;
        out[3 * g1::Toe[i] + 2] = ankle[2] + 0.15f;
    }
}

// Walks the synthetic G1 for four seconds at 30 frames a second, the player standing still while the
// game world slides past at the walk's speed in this body, and measures each planted foot against the
// world: how far it drifts within one stance, how far its ankle is from its floor height, its toe from
// the floor, and how far each leg bone is from its length.
struct GaitMeasure
{
    float worstSlip = 0.0f;
    float worstAnkleHeight = 0.0f;
    float worstToe = 0.0f;
    float worstBone = 0.0f;
    int plantedFrames = 0;
};

GaitMeasure walkSynthetic(const ComposeOptions& options)
{
    AgentState s;
    BodyModel body;
    LegState legs;
    GaitMeasure m;
    const float k = body.scaleFor(1.6f);
    float frame[g1::Joints * 3];
    bool stance[2] = {false, false};
    float start[2][3] = {};
    bool wasPlanted[2] = {false, false};
    for (int f = 0; f < 120; ++f)
    {
        const float t = 0.5f + float(f) / 30.0f;
        syntheticG1(t, frame, stance[0], stance[1]);
        if (f == 0)
            legs = LegState{};
        const float s0 = f == 0 ? 1.0f : legScale(s, legs, body);
        // Forward is -Z, so the world slides towards +Z as the player walks.
        const float move[3] = {0.0f, 0.0f, s0 * 0.8f * t};
        const SimBody b = composeBody(s, frame, move, legs, body, options);
        for (int i = 0; i < 2; ++i)
        {
            const int ankle = i == 0 ? oxr::protocol::BODY_LEFT_ANKLE : oxr::protocol::BODY_RIGHT_ANKLE;
            const bool planted = stance[i] && f > 0 && (!options.plantFeet || (b.contact & (1u << i)) != 0);
            float world[3] = {b.joints[ankle][0] - move[0], b.joints[ankle][1] - move[1], b.joints[ankle][2] - move[2]};
            if (planted && !wasPlanted[i])
                std::memcpy(start[i], world, sizeof(world));
            if (planted && f > 1)
            {
                const float dx = world[0] - start[i][0];
                const float dz = world[2] - start[i][2];
                m.worstSlip = std::max(m.worstSlip, std::sqrt(dx * dx + dz * dz));
                m.worstAnkleHeight = std::max(m.worstAnkleHeight, std::abs(b.joints[ankle][1] - k * body.ankleHeight));
                m.worstToe = std::max(m.worstToe, std::abs(b.joints[ankle + 1][1]));
                ++m.plantedFrames;
            }
            wasPlanted[i] = planted;
            const int hip = ankle - 2;
            m.worstBone = std::max(m.worstBone, std::abs(distance3(b.joints[hip], b.joints[hip + 1]) - k * body.thigh));
            m.worstBone = std::max(m.worstBone, std::abs(distance3(b.joints[hip + 1], b.joints[ankle]) - k * body.shin));
        }
    }
    return m;
}

void check(bool ok, const char* what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

// A frame of `count` full packets, each byte its packet index.
std::vector<std::vector<uint8_t>> packets(uint16_t count)
{
    std::vector<std::vector<uint8_t>> out;
    for (uint16_t i = 0; i < count; ++i)
        out.emplace_back(oxr::protocol::MAX_PACKET_PAYLOAD, uint8_t(i + 1));
    return out;
}

oxr::protocol::VideoPacketHeader header(uint32_t frame, uint16_t index, uint16_t total, uint16_t size, bool fec = false)
{
    oxr::protocol::VideoPacketHeader h = {};
    h.frameIndex = frame;
    h.packetIndex = index;
    h.totalPackets = total;
    h.payloadSize = size;
    h.flags = fec ? oxr::protocol::VIDEO_FLAG_FEC : 0;
    return h;
}

std::vector<uint8_t> parity(const std::vector<std::vector<uint8_t>>& data, uint32_t group, uint16_t total)
{
    const oxr::fec::GroupLayout layout{total, false};
    const uint32_t start = layout.Member(group, 0);
    const uint32_t end = start + layout.MemberCount(group);
    std::vector<uint8_t> out(oxr::protocol::MAX_PACKET_PAYLOAD, 0);
    for (uint32_t i = start; i < end; ++i)
        for (size_t b = 0; b < out.size(); ++b)
            out[b] ^= data[i][b];
    return out;
}

size_t deliverAllBut(VideoFrameAssembler& a, const std::vector<std::vector<uint8_t>>& data, uint16_t total,
                     std::vector<uint16_t> skip, bool withParity, std::vector<uint8_t>* nal)
{
    size_t delivered = 0;
    const std::function<void(std::vector<AssembledVideoFrame>)> take = [&](std::vector<AssembledVideoFrame> frames) {
        for (AssembledVideoFrame& f : frames)
        {
            ++delivered;
            if (nal != nullptr)
                *nal = f.nalUnit;
        }
    };
    for (uint16_t i = 0; i < total; ++i)
    {
        if (std::find(skip.begin(), skip.end(), i) != skip.end())
            continue;
        const oxr::protocol::VideoPacketHeader h = header(7, i, total, uint16_t(data[i].size()));
        take(a.addPacket(h, reinterpret_cast<const char*>(data[i].data()), std::ptrdiff_t(data[i].size()), 1));
    }
    if (withParity)
    {
        for (uint32_t g = 0; g < oxr::fec::GroupCount(total); ++g)
        {
            const std::vector<uint8_t> p = parity(data, g, total);
            oxr::protocol::VideoPacketHeader h = header(7, uint16_t(g), total, uint16_t(p.size()), true);
            h.fecGroupLastPacketPayloadSize = uint16_t(oxr::protocol::MAX_PACKET_PAYLOAD);
            take(a.addPacket(h, reinterpret_cast<const char*>(p.data()), std::ptrdiff_t(p.size()), 1));
        }
    }
    take(a.expirePendingFrame(10'000'000'000, 1));
    return delivered;
}

// The parts of a PNG reader the round trip needs: chunk CRCs, stored deflate blocks, filter 0 rows.
bool readStoredPng(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba, int& w, int& h)
{
    auto u32 = [&](size_t at) {
        return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | png[at + 3];
    };
    if (png.size() < 8 || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0)
        return false;
    std::vector<uint8_t> z;
    for (size_t at = 8; at + 12 <= png.size();)
    {
        const uint32_t len = u32(at);
        if (at + 12 + len > png.size() || crc32(png.data() + at + 4, len + 4) != u32(at + 8 + len))
            return false;
        const std::string type(reinterpret_cast<const char*>(png.data() + at + 4), 4);
        if (type == "IHDR")
        {
            w = int(u32(at + 8));
            h = int(u32(at + 12));
        }
        else if (type == "IDAT")
        {
            z.insert(z.end(), png.begin() + long(at + 8), png.begin() + long(at + 8 + len));
        }
        at += 12 + len;
    }
    std::vector<uint8_t> raw;
    for (size_t at = 2; at + 5 <= z.size();)
    {
        const bool last = z[at] & 1;
        const size_t len = size_t(z[at + 1]) | (size_t(z[at + 2]) << 8);
        raw.insert(raw.end(), z.begin() + long(at + 5), z.begin() + long(at + 5 + len));
        at += 5 + len;
        if (last)
            break;
    }
    const size_t stride = size_t(w) * 4;
    if (raw.size() != (stride + 1) * size_t(h))
        return false;
    rgba.clear();
    for (int y = 0; y < h; ++y)
        rgba.insert(rgba.end(), raw.begin() + long(y * (stride + 1) + 1), raw.begin() + long((y + 1) * (stride + 1)));
    return true;
}

std::vector<uint8_t> gradient(int w, int h)
{
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = uint8_t(i * 7);
    return rgba;
}

const std::map<std::string, std::function<void()>> cases = {
    {"assembler.complete-frame",
     [] {
         VideoFrameAssembler a;
         std::vector<uint8_t> nal;
         check(deliverAllBut(a, packets(5), 5, {}, false, &nal) == 1, "a complete frame is delivered once");
         check(nal.size() == 5 * oxr::protocol::MAX_PACKET_PAYLOAD && nal.back() == 5, "packets are joined in order");
     }},
    {"assembler.fec-recovers-one-loss",
     [] {
         VideoFrameAssembler a;
         std::vector<uint8_t> nal;
         check(deliverAllBut(a, packets(5), 5, {2}, true, &nal) == 1, "one lost packet is rebuilt from parity");
         check(nal.size() > 3 * oxr::protocol::MAX_PACKET_PAYLOAD && nal[2 * oxr::protocol::MAX_PACKET_PAYLOAD] == 3,
               "the rebuilt packet carries its bytes");
         check(a.fecRecoveries() == 1, "the recovery is counted");
     }},
    {"assembler.two-losses-drop",
     [] {
         VideoFrameAssembler a;
         check(deliverAllBut(a, packets(5), 5, {1, 2}, true, nullptr) == 0, "two losses in one group cannot be rebuilt");
         check(a.droppedFrames() == 1, "the frame is counted as dropped");
     }},
    {"assembler.render-pose-joins-frame",
     [] {
         // Frame 7's pose arrives before its packets, frame 8's after; frame 9 gets none.
         const auto pose = [](VideoFrameAssembler& a, uint32_t frame, float x) {
             oxr::protocol::VideoPacketHeader h = {};
             h.frameIndex = frame;
             h.flags = oxr::protocol::VIDEO_FLAG_RENDER_POSE;
             const float p[7] = {x, 1.5f, -0.25f, 0.0f, 0.0f, 0.0f, 1.0f};
             h.payloadSize = sizeof(p);
             check(a.addPacket(h, reinterpret_cast<const char*>(p), sizeof(p), 1).empty(),
                   "a render-pose packet delivers no frame");
         };
         const auto frameOf = [](VideoFrameAssembler& a, uint32_t frame) {
             const std::vector<uint8_t> data(64, uint8_t(frame));
             std::vector<AssembledVideoFrame> out = a.addPacket(header(frame, 0, 1, 64),
                 reinterpret_cast<const char*>(data.data()), 64, 1);
             check(out.size() == 1, "a one-packet frame is delivered");
             return out.empty() ? AssembledVideoFrame{} : out.front();
         };
         VideoFrameAssembler a;
         pose(a, 7, 0.5f);
         const AssembledVideoFrame seven = frameOf(a, 7);
         check(seven.hasRenderPose && seven.renderPosition[0] == 0.5f && seven.renderPosition[1] == 1.5f &&
                   seven.renderOrientation[3] == 1.0f,
               "a pose sent first rides on its frame");
         AssembledVideoFrame eight = frameOf(a, 8);
         check(!eight.hasRenderPose, "a frame whose pose has not arrived carries none");
         pose(a, 8, -0.5f);
         check(a.attachRenderPose(eight) && eight.renderPosition[0] == -0.5f, "a pose sent after is joined late");
         AssembledVideoFrame nine = frameOf(a, 9);
         check(!a.attachRenderPose(nine) && !nine.hasRenderPose, "another frame's pose is never borrowed");
         // The runtime's encoder restarts: index 9 comes again with a later time, and the old pose must not join.
         {
             oxr::protocol::VideoPacketHeader h = {};
             h.frameIndex = 10;
             h.presentationTimeNs = 100;
             h.flags = oxr::protocol::VIDEO_FLAG_RENDER_POSE;
             const float p[7] = {9.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
             h.payloadSize = sizeof(p);
             a.addPacket(h, reinterpret_cast<const char*>(p), sizeof(p), 1);
             AssembledVideoFrame restarted;
             restarted.frameIndex = 10;
             restarted.presentationTimeNs = 200;
             check(!a.attachRenderPose(restarted), "a pose for the same index at another time is refused");
             restarted.presentationTimeNs = 100;
             check(a.attachRenderPose(restarted) && restarted.renderPosition[0] == 9.0f, "the matching time joins");
         }
         a.clearRenderPoses();
         AssembledVideoFrame again;
         again.frameIndex = 7;
         check(!a.attachRenderPose(again), "a new connection forgets the old poses");
     }},
    {"assembler.stale-packet-never-joins-a-frame",
     [] {
         // After an encoder restart, index 5 is sent again at time 200; a late packet of the old index 5,
         // sent at time 100, must not fill a slot of the new frame.
         VideoFrameAssembler a;
         const std::vector<uint8_t> fresh(64, 0xA1);
         const std::vector<uint8_t> stale(64, 0x5E);
         oxr::protocol::VideoPacketHeader first = header(5, 0, 2, 64);
         first.presentationTimeNs = 200;
         check(a.addPacket(first, reinterpret_cast<const char*>(fresh.data()), 64, 1).empty(), "half a frame waits");
         oxr::protocol::VideoPacketHeader old = header(5, 1, 2, 64);
         old.presentationTimeNs = 100;
         check(a.addPacket(old, reinterpret_cast<const char*>(stale.data()), 64, 2).empty(),
               "a stale packet completes no frame");
         oxr::protocol::VideoPacketHeader second = header(5, 1, 2, 64);
         second.presentationTimeNs = 200;
         const std::vector<AssembledVideoFrame> out =
             a.addPacket(second, reinterpret_cast<const char*>(fresh.data()), 64, 3);
         check(out.size() == 1 && out.front().presentationTimeNs == 200 &&
                   std::all_of(out.front().nalUnit.begin(), out.front().nalUnit.end(),
                               [](uint8_t b) { return b == 0xA1; }),
               "the frame holds only its own bytes");
         // The same index again after delivery, at a later time, is a new frame, not parity of the old one.
         oxr::protocol::VideoPacketHeader again = header(5, 0, 1, 64);
         again.presentationTimeNs = 300;
         check(a.addPacket(again, reinterpret_cast<const char*>(fresh.data()), 64, 4).size() == 1,
               "a restarted index after delivery is delivered");
     }},
    {"png.round-trip",
     [] {
         const std::vector<uint8_t> pixels = gradient(37, 23);
         std::vector<uint8_t> back;
         int w = 0;
         int h = 0;
         check(readStoredPng(encodePng(pixels.data(), 37, 23), back, w, h), "the PNG parses");
         check(w == 37 && h == 23 && back == pixels, "pixels survive the round trip");
     }},
    {"png.corrupt-crc-rejected",
     [] {
         const std::vector<uint8_t> pixels = gradient(8, 8);
         std::vector<uint8_t> png = encodePng(pixels.data(), 8, 8);
         png[40] ^= 0x01; // inside IDAT, so its CRC no longer matches
         std::vector<uint8_t> back;
         int w = 0;
         int h = 0;
         check(!readStoredPng(png, back, w, h), "a corrupted chunk is rejected");
     }},
    {"agent.trigger-reaches-packet",
     [] {
         AgentState s;
         ClientStatus status;
         check(runCommand("trigger right 1", s, status, 0).reply == "{\"ok\":true}", "trigger accepted");
         oxr::protocol::TrackingPacket p;
         fillTrackingPacket(s, 1, p);
         check(p.rightTrigger == 1.0f && (p.buttonState & oxr::protocol::BUTTON_RIGHT_TRIGGER) != 0,
               "the right trigger is pulled in the packet");
         check(p.leftTrigger == 0.0f, "the left trigger is untouched");
     }},
    {"agent.release-clears-input",
     [] {
         AgentState s;
         ClientStatus status;
         runCommand("trigger right 1", s, status, 0);
         runCommand("button a 1", s, status, 0);
         runCommand("head 1 1.7 2 0 0 1 0 1 0 -1 0 0", s, status, 0);
         runCommand("release", s, status, 0);
         oxr::protocol::TrackingPacket p;
         fillTrackingPacket(s, 1, p);
         check(p.rightTrigger == 0.0f && p.buttonState == 0, "release lets go of every input");
         check(s.head.rotation.m[2] == 1.0f && s.head.rotation.m[6] == -1.0f, "release keeps where the head looks");
     }},
    {"agent.absent-hands-clear-flags",
     [] {
         AgentState s;
         ClientStatus status;
         oxr::protocol::TrackingPacket p;
         fillTrackingPacket(s, 1, p);
         check((p.trackingFlags & oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE) != 0, "hands start present");
         runCommand("present right 0", s, status, 0);
         fillTrackingPacket(s, 1, p);
         check((p.trackingFlags & oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE) == 0, "an absent hand clears its flag");
     }},
    {"agent.headset-samples-keep-their-time",
     [] {
         // A headset at 72 Hz turns at 2 rad/s about +Y; the tracking thread ticks at 90 Hz. The runtime predicts
         // from consecutive packets, so every pair it sees must give the turn rate, not 0 or twice it.
         constexpr double omega = 2.0;
         constexpr int64_t headsetNs = 1'000'000'000 / 72;
         constexpr int64_t tickNs = 1'000'000'000 / 90;
         AgentState s;
         int64_t last = 0;
         std::vector<oxr::protocol::TrackingPacket> sent;
         int64_t nextSample = 3'000'000;
         for (int64_t now = 0; now < 2'000'000'000; now += tickNs)
         {
             while (nextSample <= now)
             {
                 const double yaw = omega * double(nextSample) * 1e-9;
                 const float q[4] = {0.0f, float(std::sin(yaw * 0.5)), 0.0f, float(std::cos(yaw * 0.5))};
                 fromQuaternion(q, s.head.rotation);
                 s.headSampleNs = nextSample;
                 s.headAngularVelocity[1] = float(omega);
                 nextSample += headsetNs;
             }
             oxr::protocol::TrackingPacket p;
             if (nextTrackingPacket(s, now, last, p))
                 sent.push_back(p);
         }
         check(sent.size() > 100, "packets went out");
         double worst = 0.0;
         for (size_t i = 1; i < sent.size(); ++i)
         {
             const float* a = sent[i - 1].headOrientation;
             const float* b = sent[i].headOrientation;
             const double dot = std::fabs(double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2] + double(a[3]) * b[3]);
             const double angle = 2.0 * std::acos(std::min(1.0, dot));
             const double dt = double(sent[i].timestampNs - sent[i - 1].timestampNs) * 1e-9;
             const double rate = dt > 0.0 ? angle / dt : 0.0;
             worst = std::max(worst, std::fabs(rate - omega) / omega);
         }
         std::printf("worst turn-rate error between packets %.1f%%\n", worst * 100.0);
         check(worst < 0.02, "every packet pair gives the head's turn rate");
         check(sent.back().headAngularVelocity[1] == float(omega), "the headset's angular velocity reaches the packet");
     }},
    {"agent.unsampled-head-sends-every-tick",
     [] {
         AgentState s;
         int64_t last = 0;
         oxr::protocol::TrackingPacket p;
         check(nextTrackingPacket(s, 10, last, p) && p.timestampNs == 10, "a head with no sample time goes out now");
         check(nextTrackingPacket(s, 20, last, p) && p.timestampNs == 20, "and again on the next tick");
     }},
    {"client.stays-with-a-streaming-runtime",
     [] {
         constexpr int64_t limit = 3'000'000'000;
         check(!shouldConnectOnAnnounce(true, false, true, 10'000'000, limit),
               "another runtime's announce does not take a client whose video flows");
         check(shouldConnectOnAnnounce(true, false, true, limit + 1, limit), "a silent runtime is left");
         check(shouldConnectOnAnnounce(false, true, true, 0, limit), "connectNow moves the client");
         check(shouldConnectOnAnnounce(true, false, false, 0, limit), "auto-connect joins the first runtime");
         check(!shouldConnectOnAnnounce(false, false, false, 0, limit), "a disconnected client stays away");
     }},
    {"client.local-runtime-wins",
     [] {
         // A pilot on a desk with SteamVR followed the first runtime it heard, which could be another
         // machine's: the game on this PC then rendered to no one, a black view.
         constexpr int64_t s = 1'000'000'000;
         AnnounceContext c;
         c.listeningNs = 10 * s;
         c.announceIsLocal = true;
         check(shouldConnectToAnnounce(c, 3 * s, s), "an idle client takes this machine's runtime");
         c.announceIsLocal = false;
         c.sinceLocalAnnounceNs = 200'000'000;
         check(!shouldConnectToAnnounce(c, 3 * s, s), "not another machine's while this one's announces");
         c.sinceLocalAnnounceNs = INT64_MAX;
         c.listeningNs = 200'000'000;
         check(!shouldConnectToAnnounce(c, 3 * s, s), "nor in the first second, before this one's is heard");
         c.listeningNs = 10 * s;
         check(shouldConnectToAnnounce(c, 3 * s, s), "with no runtime here, another machine's is taken");
         c.connected = true;
         c.connectedToLocal = false;
         c.announceIsLocal = true;
         check(shouldConnectToAnnounce(c, 3 * s, s), "this machine's runtime takes over from another's");
         c.connectedToLocal = true;
         c.announceIsLocal = false;
         c.videoSilenceNs = 60 * s;
         check(!shouldConnectToAnnounce(c, 3 * s, s), "another machine never takes this one's session");
         c.connectRequested = true;
         check(shouldConnectToAnnounce(c, 3 * s, s), "a connect the user asked for is kept");
         AnnounceContext off;
         off.autoConnect = false;
         off.announceIsLocal = true;
         off.listeningNs = 10 * s;
         check(!shouldConnectToAnnounce(off, 3 * s, s), "--no-autoconnect waits for the user");
     }},
    {"client.local-address-is-recognised",
     [] {
         const auto ip = [](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
             const uint8_t bytes[4] = {a, b, c, d};
             uint32_t networkOrder = 0;
             std::memcpy(&networkOrder, bytes, 4);
             return networkOrder;
         };
         check(isLocalAddress(ip(127, 0, 0, 1)), "127.0.0.1 is this machine");
         check(!isLocalAddress(ip(192, 0, 2, 1)), "192.0.2.1 (TEST-NET-1) is not");
     }},
    {"mouse.look-turns-evenly",
     [] {
         // A steady 1000 Hz mouse, read by a 60 Hz window, sampled by the 90 Hz tracking thread: every
         // tick should turn the head by the same amount, as WASD walks it.
         for (const int windowHz : {60, 144})
         {
             MouseLook look;
             const int64_t window = 1'000'000'000 / windowHz, tick = 1'000'000'000 / 90;
             int64_t nextRead = window, nextTick = tick, mouse = 0;
             std::vector<float> steps;
             for (int64_t t = 0; t < 2'000'000'000; t += 100'000)
             {
                 if (t >= nextRead)
                 {
                     // Everything the mouse did since the last read arrives together.
                     for (; mouse + 1'000'000 <= t; mouse += 1'000'000)
                         look.push(mouse + 1'000'000, t, 1.0f, 0.0f);
                     nextRead += window;
                 }
                 if (t >= nextTick)
                 {
                     float dx = 0.0f, dy = 0.0f;
                     look.take(t, dx, dy);
                     if (t > 300'000'000)
                         steps.push_back(dx);
                     nextTick += tick;
                 }
             }
             float lo = 1e9f, hi = 0.0f, sum = 0.0f;
             for (float s : steps)
                 lo = std::min(lo, s), hi = std::max(hi, s), sum += s;
             const float mean = sum / float(steps.size());
             std::printf("window %d Hz: per tick min %.1f mean %.1f max %.1f px, delay %.1f ms\n", windowHz, lo, mean, hi,
                         double(look.delayNs()) / 1e6);
             check(mean > 10.0f && mean < 12.3f, "the head turns as far as the mouse moved");
             check(hi - lo <= 2.0f, "each tick turns the head by the same amount, within the mouse's 1 ms steps");
             check(look.delayNs() <= window + 2'000'000, "the head lags the mouse by no more than one window frame");
         }
     }},
    {"mouse.look-lands-whole",
     [] {
         // Nothing is lost or doubled, a stall is caught up at once, and releasing the mouse drops the rest.
         MouseLook look;
         look.push(1'000'000, 1'000'000, 3.0f, -2.0f);
         float dx = 0.0f, dy = 0.0f;
         look.take(1'000'000 + MouseLook::MaxDelayNs, dx, dy);
         check(dx == 3.0f && dy == -2.0f, "motion is taken once whole");
         look.push(200'000'000, 900'000'000, 5.0f, 0.0f);
         dx = dy = 0.0f;
         look.take(900'000'000, dx, dy);
         check(dx == 5.0f, "motion that arrives after a long stall is taken at once");
         check(look.delayNs() <= MouseLook::MaxDelayNs, "and the lag stays bounded");
         look.push(950'000'000, 950'000'000, 7.0f, 0.0f);
         look.clear();
         dx = dy = 0.0f;
         look.take(2'000'000'000, dx, dy);
         check(dx == 0.0f, "cleared motion never turns the head");
     }},
    {"client.second-pilot-is-refused",
     [] {
         // Two pilots bound the discovery and video ports with SO_REUSEADDR; each datagram reached one
         // of them, so the one on screen went black with no error.
         // A port the system says is free: a fixed range is not, on every desk (on one, Windows
         // refused 41890 and up with no endpoint listed).
         const uint16_t port = freeUdpPort();
         check(port != 0, "the system offers a free port");
         std::string error;
         const intptr_t first = bindExclusiveUdp(port, &error);
         check(first >= 0, "the first pilot binds the port");
         std::string second;
         const intptr_t again = bindExclusiveUdp(port, &second);
         check(again < 0, "a second pilot is refused");
         check(second.find(std::to_string(port)) != std::string::npos && second.find("another XR Pilot") != std::string::npos,
               "and told the port and why");
         closeUdp(again);
         closeUdp(first);
     }},
    {"ui.marks-take-no-frame",
     [] {
         // UI actions happen with no stream; a span that waited for a frame would never finish its row.
         TraceStore store;
         std::string error;
         check(store.open(":memory:", 0, 0, &error), "the store opens");
         UiTrace ui(&store);
         ui.record(UiAction::LaunchPilot);
         ui.record(UiAction::DeskSetup);
         check(!store.wantsFrame(), "a UI span waits for no frame");
         store.begin("call-1", "screenshot", "", 1);
         check(store.wantsFrame(), "control: an MCP span does");
     }},
    {"ui.sample-run",
     [] {
         // A first start that swaps to OXRSys and goes back to SteamVR, as the app records it, kept in
         // ui-sample.sqlite for CI to export (xrpilot-ui-trace) and replay against the Lean model (ui_check).
         std::remove("ui-sample.sqlite");
         TraceStore store;
         std::string error;
         check(store.open("ui-sample.sqlite", 0, 0, &error), "the store opens");
         UiTrace ui(&store);
         for (const UiAction a : {UiAction::LaunchPilot, UiAction::DeskSetup, UiAction::FirstRunBind, UiAction::WindowOpen,
                                  UiAction::ElevatedDone, UiAction::FieldOfView, UiAction::WindowClose,
                                  UiAction::TrayShowPilot, UiAction::WindowOpen, UiAction::RuntimeOther,
                                  UiAction::ElevatedDone, UiAction::TrayQuit, UiAction::WindowClose})
             ui.record(a);
         store.close();
     }},
    {"ui.spans-export-as-lines",
     [] {
         TraceStore store;
         std::string error;
         check(store.open(":memory:", 0, 0, &error), "the store opens");
         UiTrace ui(&store);
         ui.record(UiAction::LaunchTray);
         store.begin("call-1", "screenshot", "", 1);
         ui.record(UiAction::DeskSetup);
         ui.record(UiAction::FieldOfView, true, "say \"95\"\n");
         ui.record(UiAction::ElevatedDeclined, false);
         const std::string lines = uiTraceJsonl(store.db());
         std::printf("%s", lines.c_str());
         const std::string want =
             "{\"session\":1,\"span\":\"ui.launch.tray\",\"ok\":true}\n"
             "{\"session\":1,\"span\":\"ui.tray.setup\",\"ok\":true}\n"
             "{\"session\":1,\"span\":\"ui.window.field-of-view\",\"ok\":true,\"detail\":\"say \\\"95\\\"\\u000a\"}\n"
             "{\"session\":1,\"span\":\"ui.tray.elevated-declined\",\"ok\":false}\n";
         check(lines == want, "only ui.* spans, in start order, one JSON object a line");
         check(std::string(uiSpanName(UiAction::TrayQuit)) == "ui.tray.quit", "names come from the Lean model");
     }},
    {"launch.plain-start-opens-the-pilot",
     [] {
         // The Start Menu entry starts xr-pilot with no arguments. It ran as a tray icon alone, so a first
         // user saw no window, nothing received the stream, and an app's frames went nowhere: a black view.
         const char* argv[] = {"xr-pilot.exe"};
         const LaunchOptions o = parseLaunch(1, argv, false);
         check(o.mode == LaunchMode::PilotThenTray, "a plain start opens the pilot, then keeps the tray");
         check(o.autoConnect, "and connects to the first runtime it hears");
         check(ownsDesk(o.mode), "and sets up the desk");
         const char* tray[] = {"xr-pilot.exe", "--tray"};
         check(parseLaunch(2, tray, false).mode == LaunchMode::TrayOnly, "--tray starts the tray alone");
         check(ownsDesk(LaunchMode::TrayOnly), "the tray alone also sets up the desk");
     }},
    {"launch.second-start-shows-the-first",
     [] {
         // A second start ran a second pilot beside the first; both bound the discovery and video ports.
         const char* argv[] = {"xr-pilot.exe"};
         check(parseLaunch(1, argv, true).mode == LaunchMode::ShowRunning, "a plain start shows the running pilot");
         const char* tray[] = {"xr-pilot.exe", "--tray"};
         check(parseLaunch(2, tray, true).mode == LaunchMode::ShowRunning, "so does a start at sign-in");
         check(!ownsDesk(LaunchMode::ShowRunning), "and it leaves the desk to the running copy");
         const char* agent[] = {"xr-pilot.exe", "--agent"};
         check(parseLaunch(2, agent, true).mode == LaunchMode::Agent, "an agent's pilot still runs its own");
         const char* window[] = {"xr-pilot.exe", "--window"};
         check(parseLaunch(2, window, true).mode == LaunchMode::PilotOnly, "so does a scripted pilot");
     }},
    {"launch.flags-keep-their-meaning",
     [] {
         const char* argv[] = {"xr-pilot.exe", "--window", "--no-autoconnect", "--traces", "t.sqlite",
                               "--record-stream", "r.pwrec"};
         const LaunchOptions o = parseLaunch(7, argv, false);
         check(o.mode == LaunchMode::PilotOnly, "--window is the pilot alone");
         check(!o.autoConnect, "--no-autoconnect");
         check(o.tracePath == "t.sqlite" && o.recordStreamPath == "r.pwrec", "paths kept");
         check(!ownsDesk(LaunchMode::PilotOnly) && !ownsDesk(LaunchMode::Agent), "scripted and agent pilots leave the desk alone");
         const char* agent[] = {"xr-pilot.exe", "--agent"};
         check(parseLaunch(2, agent, false).mode == LaunchMode::Agent, "--agent");
         const char* snap[] = {"xr-pilot.exe", "--snapshot", "s.png"};
         const LaunchOptions s = parseLaunch(3, snap, false);
         check(s.mode == LaunchMode::PilotOnly && s.snapshotPath == "s.png", "--snapshot is a scripted pilot");
         const char* version[] = {"xr-pilot.exe", "--version"};
         check(parseLaunch(2, version, true).mode == LaunchMode::PrintVersion, "--version prints even beside a running copy");
         const char* bad[] = {"xr-pilot.exe", "--bogus"};
         check(parseLaunch(2, bad, false).mode == LaunchMode::Usage, "an unknown flag prints usage");
         const char* both[] = {"xr-pilot.exe", "--tray", "--window"};
         check(parseLaunch(3, both, false).mode == LaunchMode::Usage, "--tray with a pilot flag prints usage");
     }},
    {"launch.mcp-flags",
     [] {
         const char* plain[] = {"xr-pilot.exe"};
         check(parseLaunch(1, plain, false).mcpPort == DefaultMcpPort, "the desk serves MCP on 9949 by default");
         const char* config[] = {"xr-pilot.exe", "--mcp-config", "--mcp-port", "9950"};
         const LaunchOptions o = parseLaunch(4, config, true);
         check(o.mode == LaunchMode::PrintMcpConfig && o.mcpPort == 9950, "--mcp-config prints, even beside a running copy");
         const char* off[] = {"xr-pilot.exe", "--tray", "--mcp-port", "0"};
         check(parseLaunch(4, off, false).mcpPort == 0, "--mcp-port 0 starts no server");
         for (const char* bad : {"abc", "65536", "-1", ""})
         {
             const char* argv[] = {"xr-pilot.exe", "--mcp-port", bad};
             check(parseLaunch(3, argv, false).mode == LaunchMode::Usage, bad);
         }
         const char* agent[] = {"xr-pilot.exe", "--mcp-config", "--agent"};
         check(parseLaunch(3, agent, false).mode == LaunchMode::Usage, "--mcp-config with a pilot flag prints usage");
     }},
    {"mcp.token-is-random-and-kept",
     [] {
         namespace fs = std::filesystem;
         const fs::path dir = fs::temp_directory_path() / ("xrpilot-mcp-" + std::to_string(monotonicNowNs()));
         const std::string a = (dir / "a" / "mcp-token").string();
         const std::string b = (dir / "b" / "mcp-token").string();
         std::string first, again, other, error;
         check(loadOrCreateMcpToken(a, first, &error), error.c_str());
         check(first.size() == 64 && first.find_first_not_of("0123456789abcdef") == std::string::npos,
               "a token is 64 hex digits");
         check(loadOrCreateMcpToken(a, again) && again == first, "the token is kept across starts");
         check(loadOrCreateMcpToken(b, other) && other != first, "control: another install draws another token");
         {
             std::ofstream broken(fs::path(a), std::ios::trunc);
             broken << "short";
         }
         check(loadOrCreateMcpToken(a, again) && again != first && again.size() == 64, "a damaged token is replaced");
         check(userOnly(a), "only this user can open the token");
         const std::string plain = (dir / "plain").string();
         std::ofstream(fs::path(plain)) << first;
         check(!userOnly(plain), "control: a file written the ordinary way is caught as readable by others");
         Json parsed;
         check(parseJson(mcpClientConfig(9949, first), parsed), "the client config is JSON");
         check(parsed.find("url")->str() == "http://127.0.0.1:9949/mcp" &&
                   parsed.find("headers")->find("Authorization")->str() == "Bearer " + first,
               "the client config names the loopback URL and the bearer token");
         std::error_code ignored;
         fs::remove_all(dir, ignored);
     }},
    {"mcp.router-waits-for-the-pilot",
     [] {
         using namespace std::chrono_literals;
         CommandRouter router;
         int asked = 0;
         std::thread opener;
         const std::string reply = router.route(
             "state",
             [&] {
                 ++asked;
                 opener = std::thread([&] { router.attach([](const std::string& line) { return "ran " + line; }); });
             },
             5000ms);
         opener.join();
         check(reply == "ran state" && asked == 1, "a closed pilot is opened once and the command runs on it");
         check(router.route("state", [&] { ++asked; }, 5000ms) == "ran state" && asked == 1,
               "an open pilot takes the command without being asked again");
         router.detach();
         const std::string none = router.route("state", [] {}, 20ms);
         check(none.find("did not open") != std::string::npos, "control: a pilot that never opens gives an error");
         router.close();
         const auto start = std::chrono::steady_clock::now();
         const std::string closed = router.route("state", [] {}, 5000ms);
         check(closed.find("quitting") != std::string::npos && std::chrono::steady_clock::now() - start < 1s,
               "a closed router refuses at once");
     }},
    {"mcp.detach-waits-for-the-command",
     [] {
         using namespace std::chrono_literals;
         CommandRouter router;
         std::atomic<bool> running{false};
         std::atomic<bool> finished{false};
         router.attach([&](const std::string&) {
             running = true;
             std::this_thread::sleep_for(100ms);
             finished = true;
             return std::string("{\"ok\":true}");
         });
         std::thread command([&] { router.route("state", [] {}, 1000ms); });
         while (!running)
             std::this_thread::yield();
         router.detach();
         check(finished, "the pilot closes only after the command it is running");
         command.join();
     }},
    {"pace.ticks-evenly-at-90hz",
     [] {
         // The tracking loop's pacing. With the 15.6 ms Windows tick, packets came in pairs 0 ms apart
         // then 15.5 ms apart, and a WASD walk moved the head in 3 cm jumps.
         constexpr int64_t period = 1'000'000'000 / 90;
         const auto monotonicNowNs = [] {
             return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
         };
         std::vector<double> ms;
         int64_t next = monotonicNowNs();
         int64_t last = next;
         for (int i = 0; i < 270; ++i)
         {
             const int64_t now = monotonicNowNs();
             if (i > 0)
                 ms.push_back(double(now - last) / 1e6);
             last = now;
             next += period;
             const int64_t wait = next - monotonicNowNs();
             if (wait > 0)
                 sleepPreciseNs(wait);
             else
                 next = monotonicNowNs();
         }
         std::sort(ms.begin(), ms.end());
         const size_t bursts = size_t(std::count_if(ms.begin(), ms.end(), [](double d) { return d < 5.0; }));
         std::printf("intervals ms p5 %.2f p50 %.2f p95 %.2f, under 5 ms: %zu of %zu\n", ms[ms.size() / 20],
                     ms[ms.size() / 2], ms[ms.size() * 19 / 20], bursts, ms.size());
         check(bursts * 20 < ms.size(), "fewer than 5% of ticks come in a burst");
         check(ms[ms.size() / 2] > 9.0 && ms[ms.size() / 2] < 13.5, "the median tick is a 90 Hz period");
     }},
    {"commands.malformed-rejected",
     [] {
         AgentState s;
         ClientStatus status;
         for (const char* bad : {"head 1 2", "head 1 2 3 4 5 6", "head 0 0 0 1 0 0 0 1 0 0 0 nan",
                                 "hand middle 0 0 0 1 0 0 0 1 0 0 0 1", "trigger right", "button jump 1", "fov 5",
                                 "head 0 0 0 1 0 0 0 1 0 0 0 1 extra"})
             check(runCommand(bad, s, status, 0).reply.rfind("{\"ok\":false", 0) == 0, bad);
     }},
    {"spans.lines-parse",
     [] {
         SpanLine s;
         check(parseSpanLine("span begin 42 click {\"x\":568,\"y\":632}", s) && s.begin && s.id == "42" &&
                   s.name == "click" && s.detail == "{\"x\":568,\"y\":632}",
               "a begin carries id, tool and arguments");
         check(parseSpanLine("span begin 7 get_state", s) && s.name == "get_state" && s.detail.empty(),
               "a begin without arguments");
         check(parseSpanLine("span end 42 error", s) && !s.begin && !s.ok, "an end carries its status");
         for (const char* bad : {"span", "span begin", "span begin 1", "span end 1", "span end 1 maybe", "span open 1 x",
                                 "state"})
             check(!parseSpanLine(bad, s), bad);
         AgentState st;
         ClientStatus status;
         check(runCommand("span begin 1 look", st, status, 0).reply == "{\"ok\":true}", "the pilot accepts a span mark");
         check(runCommand("span end 1 bogus", st, status, 0).reply.rfind("{\"ok\":false", 0) == 0,
               "a malformed span mark is refused");
     }},
    {"spans.begin-end-and-children",
     [] {
         SpanLog log;
         log.begin("1", "click", "{}", 1000);
         log.child("state", true, 1001);
         log.child("hand right 0 0 0 1 0 0 0 1 0 0 0 1", true, 1002);
         log.child("trigger right 1", false, 1100);
         std::vector<Span> spans = log.recent();
         check(spans.size() == 1 && spans[0].running() && spans[0].children.size() == 3, "an open span gathers its commands");
         log.end("1", true, 1250);
         spans = log.recent();
         check(!spans[0].running() && spans[0].durationMs(9999) == 250, "the end fixes the duration at 250 ms");
         check(!spans[0].ok, "a refused child marks its span as failed");
         log.child("stray", true, 1300);
         check(log.recent()[0].children.size() == 3, "control: a command with no open span is not attached");
         log.end("nope", true, 1400);
         check(log.recent()[0].endMs == 1250, "control: ending an unknown id changes nothing");
     }},
    {"spans.repeats-duck",
     [] {
         SpanLog log;
         for (int i = 0; i < 5; ++i)
         {
             log.begin(std::to_string(i), "look", "{}", i * 300);
             log.end(std::to_string(i), true, i * 300 + 50);
         }
         log.begin("9", "click", "{}", 2000);
         std::vector<Span> spans = log.recent();
         check(spans.size() == 2 && spans[0].count == 5 && spans[1].name == "click",
               "five looks 250 ms apart duck into one span counting five");
         SpanLog slow;
         slow.begin("a", "look", "{}", 0);
         slow.end("a", true, 10);
         slow.begin("b", "look", "{}", 10 + SpanDuckMs + 1);
         check(slow.recent().size() == 2, "control: the same tool SpanDuckMs after the last ended gets its own span");
     }},
    {"traces.etnf-maskscore-rows",
     [] {
         TraceStore store;
         std::string error;
         check(store.open(":memory:", 1700000000000, 5000, &error), "a store opens in memory");
         TraceFrame dark;
         dark.width = 32;
         dark.height = 16;
         dark.rgba.assign(32 * 16 * 4, 0);
         dark.encoded.assign(64, uint8_t(1));
         dark.encodedWidth = 64;
         dark.encodedHeight = 16;
         dark.pose.assign(TracePoseFloats, 0.0f);
         dark.sequence = 1;
         TraceFrame light = dark;
         std::fill(light.rgba.begin(), light.rgba.end(), uint8_t(255));
         light.pose = dark.pose;
         light.sequence = 2;
         light.pose[2] = -0.5f;
         store.begin("1", "plan", "{\"todo_list\":[]}", 5000);
         store.begin("2", "reach", "", 5010);
         check(store.wantsFrame(), "a span that begins wants its input frame");
         store.frame(dark);
         check(!store.wantsFrame(), "one frame serves every waiting span");
         store.command("reach left 0 1 -0.5", true, 5012);
         store.end("2", true, 5020);
         store.frame(light);
         store.begin("3", "screenshot", "", 5030);
         store.flush();
         sqlite3* db = store.db();
         const std::function<int64_t(const char*)> count = [db](const char* sql) { return countRows(db, sql); };
         check(count("SELECT count(*) FROM span") == 3, "every begun span is a row");
         check(count("SELECT count(*) FROM span_end") == 1, "only the ended span has an end");
         check(count("SELECT count(*) FROM span_parent WHERE parent_span_id = 1") == 2, "spans begun inside the plan name it as parent");
         check(count("SELECT count(*) FROM span_detail") == 1, "an empty detail is no row");
         check(count("SELECT count(*) FROM span_command WHERE span_id = 2") == 1, "the command is in the running span");
         check(count("SELECT count(*) FROM maskscore_root") == 2, "the spans with an input frame are MaskScore roots");
         check(count("SELECT count(*) FROM maskscore_input") == 4, "each root has a view and a pose input");
         check(count("SELECT count(*) FROM maskscore_candidate") == 2, "the ended span has a view and a pose candidate");
         check(count("SELECT count(*) FROM asset") == 4, "one view and one pose per frame, shared by its spans");
         check(count("SELECT count(*) FROM asset_extent") == 2, "only the views have an extent");
         check(count("SELECT count(*) FROM maskscore_score m JOIN metric USING (metric_id) "
                     "WHERE name = 'head_travel_m' AND abs(metric_value - 0.5) < 1e-6") == 1,
               "the head's half-metre step is measured on the pose axis");
         check(count("SELECT count(*) FROM maskscore_score m JOIN metric USING (metric_id) "
                     "WHERE name = 'gray_l1_8x8' AND metric_value > 0.99") == 1,
               "dark to light measures the change");
         check(count("SELECT count(*) FROM editscore_pair WHERE instruction = 'reach'") == 1,
               "the EditScore pair carries the instruction");
         check(count("SELECT count(*) FROM asset JOIN asset_kind USING (asset_kind_id) WHERE name = 'pyrowave'") == 2,
               "frames are kept as streamed");
         check(count("SELECT count(*) FROM tool") == 3, "tools are interned");
         // A span that begins and ends on one frame keeps waiting until a newer frame arrives.
         store.begin("5", "wait", "", 5050);
         store.end("5", true, 5051);
         TraceFrame third = light;
         third.sequence = 3;
         store.frame(third);
         check(store.wantsFrame(), "a span that ended on its input's frame still wants a newer one");
         TraceFrame fourth = light;
         fourth.sequence = 4;
         store.frame(fourth);
         store.flush();
         check(!store.wantsFrame(), "the next frame becomes its edit");
         check(count("SELECT count(*) FROM maskscore_input i JOIN maskscore_candidate c ON c.span_id = i.span_id "
                     "AND c.candidate_axis_id = (SELECT candidate_axis_id FROM candidate_axis WHERE name = 'view') "
                     "JOIN input_column ic ON ic.input_column_id = i.input_column_id AND ic.name = 'source_view' "
                     "WHERE i.input_asset_id = c.candidate_asset_id") == 0,
               "no span's before and after are the same frame");
         TraceFrame bare = dark;
         bare.encoded.clear();
         store.begin("4", "look", "", 5040);
         store.frame(bare);
         store.flush();
         check(store.framesWithoutStream() == 1 && count("SELECT count(*) FROM asset") == 8,
               "control: a frame without stream bytes is counted, not kept");
         check(nullColumns(db).empty(), "no column holds a NULL");
     }},
    {"traces.rectgtn-time",
     [] {
         check(isoDuration(0) == "PT0S" && isoDuration(1500) == "PT1.5S" && isoDuration(62005) == "PT1M2.005S" &&
                   isoDuration(3'600'000) == "PT1H",
               "durations are ISO 8601 as RECTGTN writes them");
         check(civilTime(1791133333123) == "2026-10-04T17:02:13.123Z", "civil time is ISO 8601 UTC");
         TraceStore store;
         std::string error;
         check(store.open(":memory:", 1791133333123, 1000, &error), "a store opens");
         store.begin("1", "look", "", 2500);
         store.end("1", true, 4000);
         store.flush();
         check(countRows(store.db(), "SELECT count(*) FROM session WHERE started_at = '2026-10-04T17:02:13.123Z'") == 1 &&
                   countRows(store.db(), "SELECT count(*) FROM span WHERE started = 'PT1.5S'") == 1 &&
                   countRows(store.db(), "SELECT count(*) FROM span_end WHERE ended = 'PT3S'") == 1,
               "a span's times are offsets from the session's civil origin");
     }},
    {"traces.null-check-control",
     [] {
         TraceStore store;
         std::string error;
         check(store.open(":memory:", 0, 0, &error), "a store opens");
         sqlite3_exec(store.db(), "CREATE TABLE planted (a INTEGER); INSERT INTO planted VALUES (NULL);", nullptr, nullptr,
                      nullptr);
         const std::vector<std::string> nulls = nullColumns(store.db());
         check(nulls.size() == 1 && nulls[0] == "planted.a", "control: a planted NULL is found");
     }},
    {"spans.nested-fold-and-expand",
     [] {
         SpanLog log;
         log.begin("p", "plan", "{}", 0);
         log.begin("m", "glance/turn", "", 1);
         log.begin("t1", "look", "{}", 2);
         log.end("t1", true, 3);
         log.begin("t2", "wait", "{}", 4);
         std::vector<Span> rows = log.visible();
         check(rows.size() == 4 && rows[1].depth == 1 && rows[2].depth == 2 && rows[1].parent == "p",
               "while the plan runs its method and tool spans show nested beneath it");
         check(rows[0].nested == 3, "the plan counts the three spans begun inside it");
         log.end("t2", true, 5);
         log.end("m", true, 6);
         log.end("p", true, 7);
         rows = log.visible();
         check(rows.size() == 2 && rows[1].name == "glance/turn", "the latest plan stays open on its method");
         log.begin("q", "screenshot", "{}", 8);
         log.end("q", true, 9);
         rows = log.visible();
         check(rows.size() == 2 && rows[0].name == "plan" && rows[1].name == "screenshot",
               "control: once another span follows, a finished plan folds everything inside it");
         log.toggle("p");
         rows = log.visible();
         check(rows.size() == 3 && rows[1].name == "glance/turn", "expanding the plan shows its method, still folded");
         log.toggle("m");
         check(log.visible().size() == 5, "expanding the method too shows its tools");
         log.toggle("p");
         check(log.visible().size() == 2, "folding the plan hides the expanded method again");
         SpanLog siblings;
         siblings.begin("a", "plan", "{}", 0);
         siblings.begin("x", "look", "{}", 1);
         siblings.end("x", true, 2);
         siblings.end("a", true, 3);
         siblings.begin("y", "look", "{}", 4);
         check(siblings.recent().size() == 3, "control: a look at the top does not duck into a look nested in a plan");
     }},
    {"body.reach-within-the-arm",
     [] {
         AgentState s; // eyes at 1.6 m, facing -Z
         BodyModel body;
         float shoulder[3];
         shoulderPosition(s, 1, body, shoulder);
         check(shoulder[0] > 0.1f && shoulder[1] < 1.45f && shoulder[2] > 0.0f, "the right shoulder is right, below and behind the eyes");
         const float target[3] = {0.25f, 1.1f, -0.35f};
         const ArmSolve arm = solveArm(s, 1, target, body);
         check(arm.reachable, "a point half a metre ahead of the right shoulder is in reach");
         float worst = 0.0f;
         for (int k = 0; k < 3; ++k)
             worst = std::max(worst, std::abs(arm.hand.position[k] - target[k]));
         check(worst < 1e-3f, "the hand lands on a reachable target");
         const float k = body.scaleFor(1.6f);
         check(std::abs(distance3(arm.shoulder, arm.elbow) - k * body.upperArm) < 1e-3f, "the upper arm keeps its length");
         check(std::abs(distance3(arm.elbow, arm.hand.position) - k * (body.forearm + body.palm)) < 1e-3f, "the forearm keeps its length");
         check(arm.elbow[1] < arm.shoulder[1], "the elbow bends down");
         check(isRotation(arm.hand.rotation), "the hand's orientation is a rotation");
         const float aim[3] = {0.0f, 0.0f, -1.0f};
         const float up[3] = {0.0f, 1.0f, 0.0f};
         float aimed[3];
         float palm[3];
         apply(arm.hand.rotation, aim, aimed);
         apply(arm.hand.rotation, up, palm);
         float reachDir[3];
         for (int i = 0; i < 3; ++i)
             reachDir[i] = (target[i] - arm.shoulder[i]) / arm.distance;
         check(aimed[0] * reachDir[0] + aimed[1] * reachDir[1] + aimed[2] * reachDir[2] > 0.999f,
               "the controller points along the reach");
         check(palm[1] > 0.8f, "the palm stays level");
     }},
    {"body.reach-past-the-arm-is-clamped",
     [] {
         AgentState s;
         BodyModel body;
         const float far[3] = {0.2f, 1.3f, -3.0f};
         const ArmSolve arm = solveArm(s, 1, far, body);
         check(!arm.reachable, "a point three metres away is out of reach");
         const float dx = arm.hand.position[0] - arm.shoulder[0];
         const float dy = arm.hand.position[1] - arm.shoulder[1];
         const float dz = arm.hand.position[2] - arm.shoulder[2];
         check(std::sqrt(dx * dx + dy * dy + dz * dz) <= body.reach(1.6f) + 1e-3f, "control: the hand stops at the arm's length");
         AgentState seated;
         setSeated(seated, true);
         check(std::abs(solveArm(seated, 1, far, body).distance - arm.distance) > 0.01f &&
                   std::abs(body.scaleFor(seated.head.position[1] + SeatedEyeDrop) - body.scaleFor(1.6f)) < 1e-6f,
               "sitting moves the shoulder but keeps the arm's length");
         ClientStatus status;
         const std::string reply = runCommand("reach right 0.25 1.1 -0.35", s, status, 0).reply;
         check(reply.find("\"reachable\":true") != std::string::npos && s.hands[1].manual, "the reach command places the hand");
     }},
    {"body.feet-planted-in-the-world",
     [] {
         const GaitMeasure m = walkSynthetic(ComposeOptions{});
         std::printf("planted frames %d, slip %.4f m, ankle %.4f m, toe %.4f m, bone %.4f m\n", m.plantedFrames,
                     m.worstSlip, m.worstAnkleHeight, m.worstToe, m.worstBone);
         check(m.plantedFrames > 60, "the walk plants its feet");
         check(m.worstSlip < 0.01f, "a planted foot moves less than 1 cm (an AAA battery's width) in the world");
         check(m.worstAnkleHeight < 1e-4f, "a planted ankle sits at its height above the floor");
         check(m.worstToe < 0.01f, "a planted toe touches the floor");
         check(m.worstBone < 1e-3f, "the thigh and shin keep ANNY's lengths");
     }},
    {"body.feet-unplanted-slide",
     [] {
         ComposeOptions loose;
         loose.plantFeet = false;
         const GaitMeasure m = walkSynthetic(loose);
         std::printf("unplanted stance frames %d, slip %.4f m\n", m.plantedFrames, m.worstSlip);
         check(m.worstSlip > 0.01f, "control: without the floor lock a stance foot slides more than 1 cm");
     }},
    {"commands.json-string-escapes",
     [] {
         check(jsonString("C:\\temp\\a \"b\".png") == "\"C:\\\\temp\\\\a \\\"b\\\".png\"",
               "backslashes and quotes are escaped");
     }},
    {"commands.unknown-rejected",
     [] {
         AgentState s;
         ClientStatus status;
         check(runCommand("teleport 1 2 3", s, status, 0).reply.find("unknown command") != std::string::npos,
               "an unknown verb is an error");
     }},
    {"json.round-trip-keeps-order-and-numbers",
     [] {
         const std::string text = "{\"steamvr\":{\"forcedDriver\":\"x\",\"supersampleScale\":1.50,\"enable\":true},"
                                  "\"list\":[\"a\\b\",null,-2e3],\"empty\":{}}";
         Json j;
         check(parseJson(text, j), "a settings file parses");
         check(j.members[0].first == "steamvr" && j.members[1].first == "list", "members keep file order");
         Json back;
         check(parseJson(writeJson(j), back), "what is written parses again");
         check(back.find("steamvr")->find("supersampleScale")->text == "1.50", "a number is kept as written");
         check(back.find("list")->items[0].str() == "a\b" && back.find("list")->items[2].text == "-2e3",
               "strings and numbers survive the round trip");
     }},
    {"json.set-and-erase",
     [] {
         Json j;
         parseJson("{\"a\":1,\"b\":2}", j);
         j.set("a", Json::string("x"));
         j.set("c", Json::string("y"));
         check(j.members.size() == 3 && j.members[0].first == "a" && j.find("a")->str() == "x",
               "set replaces in place and appends new keys");
         check(j.erase("b") && !j.find("b") && !j.erase("b"), "erase removes a key once");
     }},
    {"json.malformed-rejected",
     [] {
         Json j;
         for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "\"unterminated", "tru", "{} extra"})
             check(!parseJson(bad, j), bad);
     }},
    {"commands.state-reports-head",
     [] {
         AgentState s;
         ClientStatus status;
         runCommand("head 0 1.6 0 0 0 1 0 1 0 -1 0 0", s, status, 0);
         const std::string json = runCommand("state", s, status, 12).reply;
         check(json.find("\"rotation\":[[0,0,1],[0,1,0],[-1,0,0]]") != std::string::npos,
               "state reports the head's rotation as a matrix");
         check(json.find("\"yaw\"") == std::string::npos, "state carries no Euler angles");
         check(json.find("\"decoded\":12") != std::string::npos, "state reports decoded frames");
     }},
    {"commands.state-reports-runtime-fov",
     [] {
         AgentState s;
         s.eyeAspect = 0.9f;
         ClientStatus status;
         const std::string own = runCommand("state", s, status, 0).reply;
         check(own.find("\"half_fov_vertical\":50") != std::string::npos, "without the runtime's FOV, the pilot's own 100 degrees");
         const float fixed[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
         std::copy(fixed, fixed + 4, s.renderTangents);
         const std::string runtime = runCommand("state", s, status, 0).reply;
         check(runtime.find("\"half_fov_vertical\":45") != std::string::npos &&
                   runtime.find("\"half_fov_horizontal\":45") != std::string::npos,
               "tangents of 1 report 45 degrees each way");
         oxr::protocol::TrackingPacket p;
         fillTrackingPacket(s, 1, p);
         check(std::abs(p.eyeFov[2] - 0.785398f) < 1e-4f && std::abs(p.eyeFov[0] + 0.785398f) < 1e-4f,
               "the packet sends the runtime's field of view");
     }},
    {"rotation.euler-orders-match-axis-products",
     [] {
         // Each order is the product of its axis rotations, checked against a vector it turns.
         const float v[3] = {0.0f, 0.0f, -1.0f};
         float out[3];
         apply(fromEuler(EulerOrder::YXZ, 90.0f, 0.0f, 0.0f), v, out);
         check(std::abs(out[0] + 1.0f) < 1e-5f && std::abs(out[2]) < 1e-5f, "yaw 90 turns forward to -X");
         apply(fromEuler(EulerOrder::YXZ, 0.0f, 30.0f, 0.0f), v, out);
         check(std::abs(out[1] - 0.5f) < 1e-5f, "pitch 30 raises forward by sin 30");
         const Rotation yxz = fromEuler(EulerOrder::YXZ, 40.0f, -25.0f, 10.0f);
         const Rotation zxy = fromEuler(EulerOrder::ZXY, 10.0f, -25.0f, 40.0f);
         check(std::abs(yxz.m[1] - zxy.m[1]) > 1e-3f, "YXZ and ZXY with the same angles differ");
         EulerOrder order;
         int parsed = 0;
         for (const char* name : {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"})
             parsed += parseEulerOrder(name, order) && isRotation(fromEuler(order, 33.0f, -71.0f, 12.0f)) ? 1 : 0;
         check(parsed == 6, "all six Tait-Bryan orders parse and give rotations");
         check(!parseEulerOrder("XYX", order) && !parseEulerOrder("yxz", order), "proper Euler and lower case are refused");
         float yaw, pitch, roll;
         toYawPitchRoll(yxz, yaw, pitch, roll);
         check(std::abs(yaw - 40.0f) < 1e-3f && std::abs(pitch + 25.0f) < 1e-3f && std::abs(roll - 10.0f) < 1e-3f,
               "YXZ angles come back out of the matrix");
     }},
    {"rotation.quaternion-round-trip",
     [] {
         for (EulerOrder order : {EulerOrder::XYZ, EulerOrder::ZYX, EulerOrder::YXZ})
         {
             const Rotation r = fromEuler(order, 120.0f, 50.0f, -170.0f);
             float q[4];
             toQuaternion(r, q);
             Rotation back;
             check(fromQuaternion(q, back), "the quaternion converts back");
             float worst = 0.0f;
             for (int i = 0; i < 9; ++i)
                 worst = std::max(worst, std::abs(back.m[i] - r.m[i]));
             check(worst < 1e-5f, "matrix to quaternion to matrix is exact to 1e-5");
         }
         const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
         Rotation unused;
         check(!fromQuaternion(zero, unused), "a zero quaternion is refused");
     }},
    {"rotation.non-rotations-refused",
     [] {
         check(isRotation(Rotation()), "the identity is a rotation");
         const Rotation scaled = {{2.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}};
         const Rotation mirrored = {{-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}};
         const Rotation sheared = {{1.0f, 0.2f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}};
         check(!isRotation(scaled) && !isRotation(mirrored) && !isRotation(sheared),
               "scale, mirror and shear are refused");
     }},
    {"commands.head-takes-a-rotation-matrix",
     [] {
         AgentState s;
         ClientStatus status;
         check(runCommand("head 0 1.6 0 0 0 1 0 1 0 -1 0 0", s, status, 0).reply == "{\"ok\":true}",
               "a rotation matrix is accepted");
         check(runCommand("head 0 1.6 0 1 0 0 0 1 0 0 0 -1", s, status, 0).reply.rfind("{\"ok\":false", 0) == 0,
               "a mirror is refused");
         check(s.head.rotation.m[2] == 1.0f, "a refused command leaves the head where it was");
         check(runCommand("hand left 0 1 0 0 0 1 0 1 0 -1 0 0", s, status, 0).reply == "{\"ok\":true}" &&
                   s.hands[0].manual && s.hands[0].pose.rotation.m[2] == 1.0f,
               "a hand takes a rotation matrix");
     }},
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || cases.count(argv[1]) == 0)
    {
        std::fprintf(stderr, "usage: xrpilot-core-tests <case>\n");
        return 2;
    }
    cases.at(argv[1])();
    return failures == 0 ? 0 : 1;
}
