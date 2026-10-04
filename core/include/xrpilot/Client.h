// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// An OXRSys client, as a headset or the simulator is: it finds a runtime by its UDP announce, connects
// asking for PyroWave, streams the agent's tracking at 90 Hz and assembles video frames.

#pragma once

#include "xrpilot/Agent.h"
#include "xrpilot/FrameAssembler.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace xrpilot
{

struct ClientStatus
{
    bool connected = false;
    std::string server;
    std::string serverName;
    uint32_t renderWidth = 0;  // per eye
    uint32_t renderHeight = 0;
    uint32_t refreshHz = 0;
    uint64_t trackingSent = 0;
    uint64_t videoPackets = 0;
    uint64_t framesAssembled = 0;
    uint64_t framesDropped = 0;
};

class Client final
{
public:
    // onFrame runs on the network thread whenever a new frame is assembled.
    explicit Client(std::function<void()> onFrame);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool start(std::string* error);
    void stop();

    // The newest assembled frame not yet taken, if any.
    std::optional<AssembledVideoFrame> takeFrame();
    void requestKeyframe();

    AgentState agent();
    void setAgent(const AgentState& state);
    ClientStatus status();

private:
    void discoveryLoop();
    void videoLoop();
    void trackingLoop();
    void connectTo(uint32_t address, const oxr::protocol::ServerAnnounce& announce);

    std::function<void()> onFrame_;
    std::atomic<bool> running_{false};
    std::thread discovery_;
    std::thread video_;
    std::thread tracking_;
    intptr_t discoverySocket_ = -1;
    intptr_t videoSocket_ = -1;
    intptr_t sendSocket_ = -1;

    std::mutex mutex_;
    AgentState agent_;
    ClientStatus status_;
    uint32_t serverAddress_ = 0; // network byte order
    std::optional<AssembledVideoFrame> latest_;
    int64_t lastVideoNs_ = 0;
    int64_t lastKeyframeRequestNs_ = 0;
    VideoFrameAssembler assembler_;
};

int64_t monotonicNowNs();

} // namespace xrpilot
