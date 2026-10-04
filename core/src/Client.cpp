// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Client.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLength = int;
using NativeSocket = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketLength = socklen_t;
using NativeSocket = int;
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace xrpilot
{

namespace
{

constexpr int64_t TrackingPeriodNs = 11'111'111; // 90 Hz
constexpr int64_t VideoSilenceReconnectNs = 3'000'000'000;

#if defined(_WIN32)
void closeSocket(intptr_t s)
{
    closesocket(NativeSocket(s));
}
#else
void closeSocket(intptr_t s)
{
    close(NativeSocket(s));
}
#endif

void setReceiveTimeout(intptr_t s, int milliseconds)
{
#if defined(_WIN32)
    const DWORD value = DWORD(milliseconds);
    setsockopt(NativeSocket(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    timeval value{milliseconds / 1000, (milliseconds % 1000) * 1000};
    setsockopt(NativeSocket(s), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
}

intptr_t openUdp(uint16_t bindPort, int receiveBuffer, std::string* error)
{
    const NativeSocket native = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const intptr_t s = intptr_t(native);
    if (s < 0 || native == NativeSocket(~0))
    {
        *error = "socket() failed";
        return -1;
    }
    const int one = 1;
    setsockopt(NativeSocket(s), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    if (receiveBuffer > 0)
        setsockopt(NativeSocket(s), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer),
                   sizeof(receiveBuffer));
    if (bindPort != 0)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(bindPort);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(NativeSocket(s), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            *error = "cannot bind UDP port " + std::to_string(bindPort);
            closeSocket(s);
            return -1;
        }
    }
    setReceiveTimeout(s, 200);
    return s;
}

void sendTo(intptr_t s, uint32_t address, uint16_t port, const void* data, size_t size)
{
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = address;
    sendto(NativeSocket(s), static_cast<const char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(&to),
           sizeof(to));
}

} // namespace

int64_t monotonicNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Client::Client(std::function<void()> onFrame)
    : onFrame_(std::move(onFrame))
{
}

Client::~Client()
{
    stop();
}

bool Client::start(std::string* error)
{
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        *error = "WSAStartup failed";
        return false;
    }
#endif
    discoverySocket_ = openUdp(oxr::protocol::DISCOVERY_PORT, 0, error);
    videoSocket_ = discoverySocket_ < 0 ? -1 : openUdp(oxr::protocol::VIDEO_PORT, 8 * 1024 * 1024, error);
    sendSocket_ = videoSocket_ < 0 ? -1 : openUdp(0, 0, error);
    if (sendSocket_ < 0)
    {
        stop();
        return false;
    }
    running_ = true;
    discovery_ = std::thread([this] { discoveryLoop(); });
    video_ = std::thread([this] { videoLoop(); });
    tracking_ = std::thread([this] { trackingLoop(); });
    return true;
}

void Client::stop()
{
    const bool wasRunning = running_.exchange(false);
    for (std::thread* t : {&discovery_, &video_, &tracking_})
    {
        if (t->joinable())
            t->join();
    }
    if (wasRunning && sendSocket_ >= 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.connected)
        {
            // Frees the runtime's client slot, as a headset does on leaving.
            const oxr::protocol::MessageType bye = oxr::protocol::MessageType::ServerDisconnect;
            sendTo(sendSocket_, serverAddress_, oxr::protocol::CONTROL_PORT, &bye, sizeof(bye));
        }
    }
    for (intptr_t* s : {&discoverySocket_, &videoSocket_, &sendSocket_})
    {
        if (*s >= 0)
            closeSocket(*s);
        *s = -1;
    }
}

void Client::connectNow()
{
    std::lock_guard<std::mutex> lock(mutex_);
    connectRequested_ = true;
}

void Client::disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_.autoConnect = false;
    connectRequested_ = false;
    if (!status_.connected)
        return;
    const oxr::protocol::MessageType bye = oxr::protocol::MessageType::ServerDisconnect;
    sendTo(sendSocket_, serverAddress_, oxr::protocol::CONTROL_PORT, &bye, sizeof(bye));
    status_.connected = false;
}

void Client::setAutoConnect(bool on)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_.autoConnect = on;
}

void Client::connectTo(uint32_t address, const oxr::protocol::ServerAnnounce& announce)
{
    oxr::protocol::ClientConnect connect = {};
    connect.type = oxr::protocol::MessageType::ClientConnect;
    connect.preferredCodec = static_cast<uint32_t>(oxr::protocol::VideoCodec::PyroWave);
    connect.maxBitrateMbps = oxr::protocol::CLIENT_MAX_BITRATE_USE_SERVER_CONFIG;
    connect.refreshRateHz = std::max<uint32_t>(announce.refreshRateHz, 60);
    std::strncpy(connect.deviceName, "OXRSys XR Pilot", sizeof(connect.deviceName) - 1);
    sendTo(sendSocket_, address, oxr::protocol::CONTROL_PORT, &connect, sizeof(connect));

    std::lock_guard<std::mutex> lock(mutex_);
    serverAddress_ = address;
    in_addr text{};
    text.s_addr = address;
    status_.connected = true;
    status_.server = inet_ntoa(text);
    status_.serverName.assign(announce.serverName, strnlen(announce.serverName, sizeof(announce.serverName)));
    status_.renderWidth = announce.renderWidth / 2;
    status_.renderHeight = announce.renderHeight;
    status_.refreshHz = announce.refreshRateHz;
    if (announce.renderHeight > 0)
        agent_.eyeAspect = float(announce.renderWidth / 2) / float(announce.renderHeight);
    // A runtime that renders a fixed field of view says so; screenshot pixels map to rays through it.
    std::copy(std::begin(announce.renderEyeTangents), std::end(announce.renderEyeTangents),
              std::begin(agent_.renderTangents));
    lastVideoNs_ = monotonicNowNs();
    connectRequested_ = false;
    assembler_.reset();
}

void Client::discoveryLoop()
{
    std::vector<uint8_t> buffer(2048);
    while (running_)
    {
        sockaddr_in from{};
        SocketLength fromLength = sizeof(from);
        const int got = recvfrom(NativeSocket(discoverySocket_), reinterpret_cast<char*>(buffer.data()),
                                 int(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (got < int(oxr::protocol::SERVER_ANNOUNCE_BASE_SIZE))
            continue;
        oxr::protocol::ServerAnnounce announce = {};
        std::memcpy(&announce, buffer.data(), std::min<size_t>(size_t(got), sizeof(announce)));
        if (announce.type != oxr::protocol::MessageType::ServerAnnounce)
            continue;
        bool reconnect = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            announcedAddress_ = from.sin_addr.s_addr;
            announced_ = announce;
            status_.discovered = true;
            if (!status_.connected)
            {
                in_addr text{};
                text.s_addr = from.sin_addr.s_addr;
                status_.server = inet_ntoa(text);
                status_.serverName.assign(announce.serverName, strnlen(announce.serverName, sizeof(announce.serverName)));
            }
            // The runtime keeps announcing while it streams; a new runtime, or one that went silent, reconnects.
            const bool wanted = status_.autoConnect || connectRequested_ || status_.connected;
            reconnect = wanted && (!status_.connected || from.sin_addr.s_addr != serverAddress_ ||
                                   monotonicNowNs() - lastVideoNs_ > VideoSilenceReconnectNs);
        }
        if (reconnect)
            connectTo(from.sin_addr.s_addr, announce);
    }
}

void Client::videoLoop()
{
    std::vector<uint8_t> buffer(oxr::protocol::VIDEO_PACKET_SIZE + 64);
    while (running_)
    {
        const int got = recv(NativeSocket(videoSocket_), reinterpret_cast<char*>(buffer.data()),
                             int(buffer.size()), 0);
        if (got < int(sizeof(oxr::protocol::VideoPacketHeader)))
            continue;
        oxr::protocol::VideoPacketHeader header = {};
        std::memcpy(&header, buffer.data(), sizeof(header));
        const std::ptrdiff_t available = got - std::ptrdiff_t(sizeof(header));
        const std::ptrdiff_t payload = std::min<std::ptrdiff_t>(header.payloadSize, available);
        const int64_t now = monotonicNowNs();
        bool delivered = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++status_.videoPackets;
            lastVideoNs_ = now;
            for (AssembledVideoFrame& frame :
                 assembler_.addPacket(header, reinterpret_cast<const char*>(buffer.data() + sizeof(header)), payload, now))
            {
                latest_ = std::move(frame);
                ++status_.framesAssembled;
                delivered = true;
            }
            status_.framesDropped = assembler_.droppedFrames();
            status_.fecRecoveries = assembler_.fecRecoveries();
        }
        if (delivered && onFrame_)
            onFrame_();
    }
}

void Client::trackingLoop()
{
    int64_t next = monotonicNowNs();
    while (running_)
    {
        next += TrackingPeriodNs;
        oxr::protocol::TrackingPacket packet;
        uint32_t address = 0;
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fillTrackingPacket(agent_, monotonicNowNs(), packet);
            address = serverAddress_;
            connected = status_.connected;
            if (connected)
                ++status_.trackingSent;
        }
        if (connected)
            sendTo(sendSocket_, address, oxr::protocol::TRACKING_PORT, &packet, sizeof(packet));
        const int64_t wait = next - monotonicNowNs();
        if (wait > 0)
            std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
        else
            next = monotonicNowNs();
    }
}

std::optional<AssembledVideoFrame> Client::takeFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<AssembledVideoFrame> frame = std::move(latest_);
    latest_.reset();
    return frame;
}

void Client::requestKeyframe(uint32_t reasonFlags, uint32_t detail)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = monotonicNowNs();
    if (!status_.connected || now - lastKeyframeRequestNs_ < 1'000'000'000)
        return;
    lastKeyframeRequestNs_ = now;
    oxr::protocol::RequestKeyframe request = {};
    request.reasonFlags = reasonFlags;
    request.detail = detail;
    sendTo(sendSocket_, serverAddress_, oxr::protocol::CONTROL_PORT, &request, sizeof(request));
}

void Client::reportLatency(const AssembledVideoFrame& frame, int64_t decodeStartNs, int64_t decodeEndNs)
{
    oxr::protocol::LatencyReport report = {};
    report.receiveToDecoderSubmitMs = float(std::max<int64_t>(0, decodeStartNs - frame.receiveTimeNs)) / 1e6f;
    report.decodeLatencyMs = float(std::max<int64_t>(0, decodeEndNs - decodeStartNs)) / 1e6f;
    report.compositorLatencyMs = 0.0f;
    report.totalClientLatencyMs = report.receiveToDecoderSubmitMs + report.decodeLatencyMs;
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.connected)
        sendTo(sendSocket_, serverAddress_, oxr::protocol::CONTROL_PORT, &report, sizeof(report));
}

void Client::updateAgent(const std::function<void(AgentState&)>& change)
{
    std::lock_guard<std::mutex> lock(mutex_);
    change(agent_);
}

AgentState Client::agent()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return agent_;
}

void Client::setAgent(const AgentState& state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    agent_ = state;
}

ClientStatus Client::status()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

} // namespace xrpilot
