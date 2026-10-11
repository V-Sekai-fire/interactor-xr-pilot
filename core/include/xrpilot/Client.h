// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// An OXRSys client, as a headset or the simulator is: it finds a runtime by its UDP announce, connects
// asking for PyroWave, streams the agent's tracking at 90 Hz and assembles video frames.

#pragma once

#include "xrpilot/Agent.h"
#include "xrpilot/MouseLook.h"
#include "xrpilot/Body.h"
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
    bool discovered = false; // a runtime has announced itself
    bool autoConnect = true;
    std::string server;
    std::string serverName;
    uint32_t renderWidth = 0;  // per eye
    uint32_t renderHeight = 0;
    uint32_t refreshHz = 0;
    uint64_t trackingSent = 0;
    uint64_t videoPackets = 0;
    uint64_t framesAssembled = 0;
    uint64_t framesDropped = 0;
    uint64_t fecRecoveries = 0;
    uint64_t motionFrames = 0; // MotionBricks frames from the motion host
    uint64_t bodySent = 0;
};

// The motion host (meshing-pen's tools/motion_host.gd) on loopback, and the speed the game's smooth
// locomotion carries the player at, which the simulated legs walk at.
constexpr uint16_t MotionHostPort = 47830;
constexpr float SmoothLocomotionSpeed = 1.5f;

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
    // Joins a render pose that arrived after takeFrame; false while it has not.
    bool attachRenderPose(AssembledVideoFrame& frame);
    // reasonFlags are oxr::protocol::KeyframeReasonFlags; at most one request a second.
    void requestKeyframe(uint32_t reasonFlags = 0, uint32_t detail = 0);
    // Tells the runtime how long the frame waited and decoded, as a headset does after each frame.
    void reportLatency(const AssembledVideoFrame& frame, int64_t decodeStartNs, int64_t decodeEndNs);

    // Connects to the runtime heard most recently, or the next to announce.
    void connectNow();
    // Leaves the runtime and stays away until connectNow or auto-connect.
    void disconnect();
    void setAutoConnect(bool on);

    AgentState agent();
    void setAgent(const AgentState& state);
    // Changes the agent state under the client's lock, so a person and the agent never overwrite each other.
    void updateAgent(const std::function<void(AgentState&)>& change);
    // Mouse look: motion the mouse made at atNs, read at arrivedNs (monotonicNowNs's clock). The
    // tracking thread turns the head by it at the pace the mouse moved (MouseLook.h).
    void pushMouse(int64_t atNs, int64_t arrivedNs, float dx, float dy) { mouseLook_.push(atNs, arrivedNs, dx, dy); }
    void clearMouse() { mouseLook_.clear(); }
    ClientStatus status();

private:
    void discoveryLoop();
    void videoLoop();
    void trackingLoop();
    void motionLoop();
    void sendBody(const AgentState& agent, const oxr::protocol::TrackingPacket& packet, uint32_t address, int64_t periodNs);
    void connectTo(uint32_t address, const oxr::protocol::ServerAnnounce& announce);

    std::function<void()> onFrame_;
    std::atomic<bool> running_{false};
    std::thread discovery_;
    std::thread video_;
    std::thread tracking_;
    std::thread motion_;
    intptr_t discoverySocket_ = -1;
    intptr_t videoSocket_ = -1;
    intptr_t sendSocket_ = -1;
    intptr_t motionSocket_ = -1;

    std::mutex mutex_;
    AgentState agent_;
    MouseLook mouseLook_;
    ClientStatus status_;
    uint32_t serverAddress_ = 0; // network byte order
    uint32_t announcedAddress_ = 0;
    oxr::protocol::ServerAnnounce announced_ = {};
    bool connectRequested_ = false;
    std::optional<AssembledVideoFrame> latest_;
    int64_t lastVideoNs_ = 0;
    int64_t listeningSinceNs_ = 0;
    int64_t lastLocalAnnounceNs_ = 0; // 0: none heard
    bool connectedToLocal_ = false;
    int64_t lastKeyframeRequestNs_ = 0;
    VideoFrameAssembler assembler_;
    // The newest MotionBricks frame, and the body composed from it on the tracking thread.
    float g1_[g1::Joints * 3] = {};
    int64_t g1AtNs_ = 0;
    LegState legs_;
    float locomotion_[3] = {0.0f, 0.0f, 0.0f};
    uint32_t steerTick_ = 0;
};

int64_t monotonicNowNs();

// Whether an announce should (re)connect the client. A connected client stays with its runtime while
// video flows, whoever else announces; it moves when asked (connectNow) or when its runtime went silent.
inline bool shouldConnectOnAnnounce(bool autoConnect, bool connectRequested, bool connected, int64_t videoSilenceNs,
                                    int64_t silenceLimitNs)
{
    if (connectRequested)
        return true;
    if (!connected)
        return autoConnect;
    return videoSilenceNs > silenceLimitNs;
}

// What the client knows when a runtime announces itself.
struct AnnounceContext
{
    bool autoConnect = true;
    bool connectRequested = false;
    bool connected = false;
    bool connectedToLocal = false;   // the runtime streaming to us runs on this machine
    bool announceIsLocal = false;    // the announcing runtime runs on this machine
    int64_t listeningNs = 0;         // since the client started listening
    int64_t sinceLocalAnnounceNs = INT64_MAX;
    int64_t videoSilenceNs = 0;
};

// The desk's own runtime wins: a game on this PC streams to this pilot, never to a runtime elsewhere
// on the network. Another machine's runtime is taken only when none runs here.
bool shouldConnectToAnnounce(const AnnounceContext& c, int64_t silenceLimitNs, int64_t localGraceNs);

// Whether an IPv4 address (network order) is this machine: loopback or one of its interfaces.
bool isLocalAddress(uint32_t address);

// Binds a UDP port that no other socket may share, so a second pilot is refused rather than
// silently taking the stream. Returns the socket, or -1 with an error naming the port.
intptr_t bindExclusiveUdp(uint16_t port, std::string* error);
void closeUdp(intptr_t s);
// The port a bound UDP socket holds (0 on error), for a socket bound to port 0.
uint16_t boundUdpPort(intptr_t s);

} // namespace xrpilot
