// SPDX-License-Identifier: MPL-2.0
//
// Ported from oxrsys clients/Qt/oxrsys-simulator-shared/src/VideoFrameAssembler.h with Qt containers
// replaced by std::vector.

#pragma once

#include <vector>

#include <cstddef>
#include <cstdint>

#include <oxrsys/protocol/Protocol.h>

struct AssembledVideoFrame
{
    std::vector<uint8_t> nalUnit;
    uint32_t frameIndex = 0;
    int64_t presentationTimeNs = 0;
    int64_t receiveTimeNs = 0;
    bool recoveredWithFec = false;
    // The head pose the runtime rendered this frame from, when its render-pose packet has arrived.
    bool hasRenderPose = false;
    float renderPosition[3] = {0.0f, 0.0f, 0.0f};
    float renderOrientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

class VideoFrameAssembler final
{
public:
    std::vector<AssembledVideoFrame> addPacket(const oxr::protocol::VideoPacketHeader& header,
                                         const char* payload,
                                         std::ptrdiff_t payloadSize,
                                         int64_t receiveTimeNs);
    std::vector<AssembledVideoFrame> expirePendingFrame(int64_t nowNs, int64_t timeoutNs);
    void reset();
    // Attaches the render pose kept for the frame, if one arrived; false when none did. A pose joins only
    // the frame with its index AND its presentation time: the runtime restarts indices when its encoder
    // starts again, and a stale datagram from before that must refuse rather than join a new frame.
    bool attachRenderPose(AssembledVideoFrame& frame) const;
    // Forgets kept render poses, as a new connection restarts frame indices.
    void clearRenderPoses();

    uint64_t droppedFrames() const;
    uint64_t fecRecoveries() const;

private:
    void startFrame(const oxr::protocol::VideoPacketHeader& header, int64_t receiveTimeNs);
    std::vector<AssembledVideoFrame> finishPendingFrame(bool countDropIfIncomplete,
                                                  int64_t receiveTimeNs);
    bool tryFecRecovery();
    bool isComplete() const;
    AssembledVideoFrame deliverPendingFrame(int64_t receiveTimeNs) const;

    uint32_t pendingFrameIndex_ = UINT32_MAX;
    uint32_t deliveredFrameIndex_ = UINT32_MAX;
    int64_t deliveredPresentationTimeNs_ = INT64_MIN;
    int64_t newestPresentationTimeNs_ = INT64_MIN;
    uint16_t pendingTotalPackets_ = 0;
    uint16_t pendingReceivedPackets_ = 0;
    int64_t pendingPresentationTimeNs_ = 0;
    int64_t pendingLastPacketTimeNs_ = 0;
    std::vector<uint8_t> pendingFrameData_;
    std::vector<uint16_t> pendingPacketSizes_;
    std::vector<uint8_t> pendingPacketReceived_;
    std::vector<uint8_t> pendingFecData_;
    std::vector<uint8_t> pendingFecReceived_;
    std::vector<uint16_t> pendingFecGroupLastPacketSizes_;
    bool pendingRecoveredWithFec_ = false;
    struct KeptRenderPose
    {
        uint32_t frameIndex = UINT32_MAX;
        int64_t presentationTimeNs = 0;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };
    static constexpr size_t KeptRenderPoses = 16;
    KeptRenderPose renderPoses_[KeptRenderPoses];
    size_t nextRenderPose_ = 0;
    uint64_t droppedFrames_ = 0;
    uint64_t fecRecoveries_ = 0;
};
