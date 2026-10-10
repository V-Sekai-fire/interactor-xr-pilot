// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Frame-sync reliability trials. Each trial is one frame the runtime sends: a render-pose datagram, then
// its video packets, through a network that drops, duplicates and reorders datagrams, replays stale ones
// from before an encoder restart, and restarts frame indices. Every frame and pose carries the frame's
// serial, so a shown frame whose pose names another serial is a sync failure. Outcomes per trial:
// committed (shown with its own pose), refused (not shown), or wrong (shown with another pose).
//
//   xrpilot-sync-trials <trials> [threads] [seed]
// prints the counts and exits 1 on any wrong pairing. Built with XRPILOT_PLANT_INDEX_ONLY (the planted
// defect: poses keyed by index alone) it must exit 1; --plant-unposed shows frames with the latest pose
// heard instead of refusing, the other planted defect.

#include "xrpilot/FrameSync.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct Datagram
{
    oxr::protocol::VideoPacketHeader header = {};
    std::vector<char> payload;
};

struct Counts
{
    uint64_t trials = 0;
    uint64_t committed = 0;
    uint64_t refused = 0;
    uint64_t wrong = 0;
};

uint64_t serialOfPose(const AssembledVideoFrame& f)
{
    uint64_t s = 0;
    std::memcpy(&s, f.renderPosition, sizeof(s)); // the serial's bits ride in x and y
    return s;
}

uint64_t serialOfNal(const AssembledVideoFrame& f)
{
    uint64_t s = UINT64_MAX;
    if (f.nalUnit.size() >= sizeof(s))
        std::memcpy(&s, f.nalUnit.data(), sizeof(s));
    return s;
}

Counts run(uint64_t trials, uint64_t seed, bool plantUnposed)
{
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    VideoFrameAssembler assembler;
    xrpilot::FrameSync sync;
    std::deque<Datagram> wire;         // in flight, in arrival order
    std::deque<Datagram> history;      // recent datagrams, replayed later as stale ones
    Counts c;
    uint32_t frameIndex = 0;
    int64_t now = 0;
    AssembledVideoFrame lastPoseSeen;  // for the planted unposed policy

    const auto attach = [&](AssembledVideoFrame& f) { return assembler.attachRenderPose(f); };
    const auto show = [&](const AssembledVideoFrame& f) {
        if (serialOfPose(f) == serialOfNal(f))
            ++c.committed;
        else
            ++c.wrong;
    };

    for (uint64_t serial = 0; serial < trials; ++serial)
    {
        now += 11'111'111; // 90 Hz
        if (u(rng) < 2e-3)
            frameIndex = 0; // the encoder starts again

        Datagram pose;
        pose.header.frameIndex = frameIndex;
        pose.header.presentationTimeNs = now;
        pose.header.flags = oxr::protocol::VIDEO_FLAG_RENDER_POSE;
        float p[7] = {0, 0, 0.5f, 0, 0, 0, 1};
        std::memcpy(p, &serial, sizeof(serial));
        pose.header.payloadSize = sizeof(p);
        pose.payload.assign(reinterpret_cast<char*>(p), reinterpret_cast<char*>(p) + sizeof(p));

        std::vector<Datagram> sent{pose};
        const uint16_t total = uint16_t(1 + rng() % 3);
        for (uint16_t i = 0; i < total; ++i)
        {
            Datagram d;
            d.header.frameIndex = frameIndex;
            d.header.packetIndex = i;
            d.header.totalPackets = total;
            d.header.presentationTimeNs = now;
            d.header.payloadSize = 16;
            d.payload.assign(16, char(i));
            std::memcpy(d.payload.data(), &serial, sizeof(serial));
            sent.push_back(std::move(d));
        }
        ++frameIndex;

        for (Datagram& d : sent)
        {
            if (u(rng) < 0.02)
                continue; // lost
            history.push_back(d);
            if (history.size() > 64)
                history.pop_front();
            wire.push_back(d);
            if (u(rng) < 0.01)
                wire.push_back(d); // duplicated
        }
        if (wire.size() >= 2 && u(rng) < 0.02)
            std::swap(wire[wire.size() - 1], wire[wire.size() - 2]); // reordered
        if (!history.empty() && u(rng) < 5e-3)
            wire.push_back(history[rng() % history.size()]); // a stale datagram turns up late

        std::optional<AssembledVideoFrame> newest;
        while (!wire.empty())
        {
            const Datagram d = std::move(wire.front());
            wire.pop_front();
            if (d.header.flags & oxr::protocol::VIDEO_FLAG_RENDER_POSE)
            {
                lastPoseSeen.hasRenderPose = true;
                std::memcpy(lastPoseSeen.renderPosition, d.payload.data(), sizeof(float) * 3);
            }
            for (AssembledVideoFrame& f : assembler.addPacket(d.header, d.payload.data(), std::ptrdiff_t(d.payload.size()), now))
                newest = std::move(f);
        }
        ++c.trials;
        if (plantUnposed)
        {
            if (newest)
            {
                if (!newest->hasRenderPose && !attach(*newest) && lastPoseSeen.hasRenderPose)
                    std::memcpy(newest->renderPosition, lastPoseSeen.renderPosition, sizeof(float) * 3);
                show(*newest);
            }
            continue;
        }
        if (std::optional<AssembledVideoFrame> shown = sync.next(std::move(newest), attach, now))
            show(*shown);
    }
    c.refused = c.trials - c.committed - c.wrong;
    return c;
}

} // namespace

int main(int argc, char** argv)
{
    bool plantUnposed = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--plant-unposed")
            plantUnposed = true;
        else
            args.emplace_back(argv[i]);
    }
    if (args.empty())
    {
        std::fprintf(stderr, "usage: xrpilot-sync-trials <trials> [threads] [seed] [--plant-unposed]\n");
        return 2;
    }
    const uint64_t trials = std::strtoull(args[0].c_str(), nullptr, 10);
    const unsigned threads = args.size() > 1 ? unsigned(std::strtoul(args[1].c_str(), nullptr, 10)) : 1;
    const uint64_t seed = args.size() > 2 ? std::strtoull(args[2].c_str(), nullptr, 10) : 1;
    std::vector<Counts> parts(threads);
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t)
    {
        const uint64_t share = trials / threads + (t < trials % threads ? 1 : 0);
        workers.emplace_back([&, t, share] { parts[t] = run(share, seed * 1000003ull + t, plantUnposed); });
    }
    for (std::thread& w : workers)
        w.join();
    Counts sum;
    for (const Counts& p : parts)
    {
        sum.trials += p.trials;
        sum.committed += p.committed;
        sum.refused += p.refused;
        sum.wrong += p.wrong;
    }
    std::printf("trials %" PRIu64 " committed %" PRIu64 " refused %" PRIu64 " wrong %" PRIu64 "\n", sum.trials,
                sum.committed, sum.refused, sum.wrong);
    if (sum.wrong == 0 && sum.trials > 0)
        std::printf("no wrong pairing: rate below %.3g at 95%% confidence (rule of three)\n", 3.0 / double(sum.trials));
    return sum.wrong == 0 ? 0 : 1;
}
