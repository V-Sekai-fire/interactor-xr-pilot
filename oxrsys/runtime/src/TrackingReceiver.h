// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "RuntimeSockets.h"

#include <oxrsys/protocol/Protocol.h>

/**
 * Receives 6DOF tracking data from the headset client via UDP.
 *
 * Runs a background thread that listens for TrackingPackets.
 * The latest pose data is stored and can be read by InputManager
 * to feed into the OpenXR runtime.
 */
class TrackingReceiver
{
public:
    TrackingReceiver() = default;
    ~TrackingReceiver();

    // Non-copyable
    TrackingReceiver(const TrackingReceiver&) = delete;
    TrackingReceiver& operator=(const TrackingReceiver&) = delete;

    bool Start();
    void Stop();

    // Get the latest tracking data (thread-safe)
    bool GetLatestPose(oxr::protocol::TrackingPacket& outPacket) const;
    bool GetPredictedPose(oxr::protocol::TrackingPacket& outPacket) const;
    // Every button bit seen down in a packet since the last call, so a press that went down and
    // up between two reads of the latest packet is not lost. Clears on return.
    uint32_t TakeButtonsPressed();

    // Inject a tracking packet from TCP (USB mode) — same effect as receiving via UDP
    void InjectPacket(const uint8_t* data, size_t size);

    // A client connected: its timestamps are on its own clock, so ordering starts again, and only packets
    // from sourceAddress (network byte order; 0 = any, as over USB) are taken.
    void BeginClient(uint32_t sourceAddress);
    static bool AcceptsSource(uint32_t expected, uint32_t from) { return expected == 0 || expected == from; }

    void SetPredictionHorizonMs(float predictionHorizonMs);
    float GetPredictionHorizonMs() const { return predictionHorizonMs_.load(); }

    // Check if we're receiving tracking data
    bool IsReceiving() const { return hasData_.load(); }
    bool IsRunning() const { return running_.load(); }

    // Stats
    uint64_t GetPacketCount() const { return packetCount_.load(); }
    uint64_t GetReorderedDropCount() const { return reorderedDropCount_.load(); }
    uint64_t GetForeignDropCount() const { return foreignDropCount_.load(); }

private:
    struct HistorySample
    {
        oxr::protocol::TrackingPacket packet = {};
        int64_t receiveTimeNs = 0;
    };

    void ReceiveThread();
    void StorePacket(const oxr::protocol::TrackingPacket& packet, int64_t receiveTimeNs);

    oxrsys::runtime_socket::SocketHandle socket_ = oxrsys::runtime_socket::InvalidSocket;
    std::thread receiveThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> hasData_{false};
    std::atomic<uint64_t> packetCount_{0};
    std::atomic<uint64_t> reorderedDropCount_{0};
    std::atomic<uint64_t> foreignDropCount_{0};
    std::atomic<uint32_t> sourceAddress_{0};

    mutable std::mutex poseMutex_;
    oxr::protocol::TrackingPacket latestPacket_ = {};
    uint32_t buttonsPressedSinceTaken_ = 0;
    std::deque<HistorySample> history_;
    std::atomic<float> predictionHorizonMs_{0.0f};
    mutable std::atomic<int64_t> lastPredictionDiagnosticNs_{0};

    static constexpr size_t MaxHistorySamples = 8;
};
