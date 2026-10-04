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
    uint64_t droppedFrames_ = 0;
    uint64_t fecRecoveries_ = 0;
};
