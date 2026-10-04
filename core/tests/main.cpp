// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Core tests, one case per ctest entry. Each negative case asserts that broken input is rejected.

#include "xrpilot/Agent.h"
#include "xrpilot/Body.h"
#include "xrpilot/Commands.h"
#include "xrpilot/FrameAssembler.h"
#include "xrpilot/Json.h"
#include "xrpilot/Png.h"
#include "xrpilot/Rotation.h"
#include "xrpilot/SpanLog.h"
#include "xrpilot/TraceStore.h"

#include <sqlite3.h>

#include <oxrsys/protocol/FecCodec.h>

#include <algorithm>
#include <cmath>
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

int64_t countRows(sqlite3* db, const char* sql)
{
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
    const int64_t n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : -1;
    sqlite3_finalize(s);
    return n;
}

float distance3(const float* p, const float* q)
{
    return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
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
         TraceFrame light = dark;
         std::fill(light.rgba.begin(), light.rgba.end(), uint8_t(255));
         light.pose = dark.pose;
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
         TraceFrame bare = dark;
         bare.encoded.clear();
         store.begin("4", "look", "", 5040);
         store.frame(bare);
         store.flush();
         check(store.framesWithoutStream() == 1 && count("SELECT count(*) FROM asset") == 4,
               "control: a frame without stream bytes is counted, not kept");
         check(nullColumns(db).empty(), "no column holds a NULL");
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
         check(rows.size() == 1 && rows[0].name == "plan", "a finished plan folds everything inside it");
         log.toggle("p");
         rows = log.visible();
         check(rows.size() == 2 && rows[1].name == "glance/turn", "expanding the plan shows its method, still folded");
         log.toggle("m");
         check(log.visible().size() == 4, "expanding the method too shows its tools");
         log.toggle("p");
         check(log.visible().size() == 1, "folding the plan hides the expanded method again");
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
