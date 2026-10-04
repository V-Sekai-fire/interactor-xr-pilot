// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Core tests, one case per ctest entry. Each negative case asserts that broken input is rejected.

#include "xrpilot/Agent.h"
#include "xrpilot/Commands.h"
#include "xrpilot/FrameAssembler.h"
#include "xrpilot/Png.h"

#include <oxrsys/protocol/FecCodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace xrpilot;

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
    uint32_t start = 0;
    uint32_t end = 0;
    oxr::fec::GroupRange(group, total, start, end);
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
    auto take = [&](std::vector<AssembledVideoFrame> frames) {
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
        const auto h = header(7, i, total, uint16_t(data[i].size()));
        take(a.addPacket(h, reinterpret_cast<const char*>(data[i].data()), std::ptrdiff_t(data[i].size()), 1));
    }
    if (withParity)
    {
        for (uint32_t g = 0; g < oxr::fec::GroupCount(total); ++g)
        {
            const std::vector<uint8_t> p = parity(data, g, total);
            auto h = header(7, uint16_t(g), total, uint16_t(p.size()), true);
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
         runCommand("head 1 1.7 2 30 -10 0", s, status, 0);
         runCommand("release", s, status, 0);
         oxr::protocol::TrackingPacket p;
         fillTrackingPacket(s, 1, p);
         check(p.rightTrigger == 0.0f && p.buttonState == 0, "release lets go of every input");
         check(s.head.yaw == 30.0f, "release keeps where the head looks");
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
    {"commands.malformed-rejected",
     [] {
         AgentState s;
         ClientStatus status;
         for (const char* bad : {"head 1 2", "head 1 2 3 4 5 nan", "hand middle 0 0 0 0 0", "trigger right",
                                 "button jump 1", "fov 5", "head 0 0 0 0 0 0 extra"})
             check(runCommand(bad, s, status, 0).reply.rfind("{\"ok\":false", 0) == 0, bad);
     }},
    {"commands.unknown-rejected",
     [] {
         AgentState s;
         ClientStatus status;
         check(runCommand("teleport 1 2 3", s, status, 0).reply.find("unknown command") != std::string::npos,
               "an unknown verb is an error");
     }},
    {"commands.state-reports-head",
     [] {
         AgentState s;
         ClientStatus status;
         runCommand("head 0 1.6 0 45 -20 0", s, status, 0);
         const std::string json = runCommand("state", s, status, 12).reply;
         check(json.find("\"yaw\":45") != std::string::npos && json.find("\"pitch\":-20") != std::string::npos,
               "state reports the head");
         check(json.find("\"decoded\":12") != std::string::npos, "state reports decoded frames");
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
