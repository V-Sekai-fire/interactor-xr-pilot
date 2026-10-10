// SPDX-License-Identifier: MPL-2.0
//
// Ported from oxrsys clients/Qt/oxrsys-simulator-shared/src/VideoFrameAssembler.cpp.

#include "xrpilot/FrameAssembler.h"

#include <oxrsys/protocol/FecCodec.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace
{

constexpr uint16_t MaxFramePackets = 4096;

std::ptrdiff_t packetOffset(uint16_t packetIndex)
{
    return static_cast<std::ptrdiff_t>(packetIndex) *
        static_cast<std::ptrdiff_t>(oxr::protocol::MAX_PACKET_PAYLOAD);
}

void appendFrames(std::vector<AssembledVideoFrame>& into, std::vector<AssembledVideoFrame> frames)
{
    for (AssembledVideoFrame& frame : frames)
    {
        into.push_back(std::move(frame));
    }
}

} // namespace

std::vector<AssembledVideoFrame> VideoFrameAssembler::addPacket(
    const oxr::protocol::VideoPacketHeader& header,
    const char* payload,
    std::ptrdiff_t payloadSize,
    int64_t receiveTimeNs)
{
    std::vector<AssembledVideoFrame> completedFrames;

    if ((header.flags & oxr::protocol::VIDEO_FLAG_RENDER_POSE) != 0)
    {
        // Seven floats: position, then orientation as x, y, z, w.
        if (payload != nullptr && payloadSize >= static_cast<std::ptrdiff_t>(7 * sizeof(float)))
        {
            KeptRenderPose& kept = renderPoses_[nextRenderPose_];
            nextRenderPose_ = (nextRenderPose_ + 1) % KeptRenderPoses;
            kept.frameIndex = header.frameIndex;
            kept.presentationTimeNs = header.presentationTimeNs;
            std::memcpy(kept.position, payload, sizeof(kept.position));
            std::memcpy(kept.orientation, payload + sizeof(kept.position), sizeof(kept.orientation));
        }
        return completedFrames;
    }
    if (header.totalPackets == 0 || header.packetIndex >= header.totalPackets ||
        header.totalPackets > MaxFramePackets)
    {
        return completedFrames;
    }

    payloadSize = std::min<std::ptrdiff_t>(
        payloadSize,
        static_cast<std::ptrdiff_t>(oxr::protocol::MAX_PACKET_PAYLOAD));

    // A frame is its index and its presentation time: the runtime restarts indices when its encoder starts
    // again, and its times only grow. A packet older than the newest frame begun is stale and never joins.
    if (header.presentationTimeNs < newestPresentationTimeNs_)
    {
        return completedFrames;
    }
    // Parity that trails a frame already delivered would otherwise start it again as a new one.
    if (header.frameIndex == deliveredFrameIndex_ && header.presentationTimeNs == deliveredPresentationTimeNs_)
    {
        return completedFrames;
    }

    const bool fecPacket = (header.flags & oxr::protocol::VIDEO_FLAG_FEC) != 0;
    const bool newFrame = pendingTotalPackets_ == 0 || header.frameIndex != pendingFrameIndex_ ||
                          header.presentationTimeNs != pendingPresentationTimeNs_;
    if (newFrame)
    {
        appendFrames(completedFrames, finishPendingFrame(true, receiveTimeNs));
        startFrame(header, receiveTimeNs);
    }

    pendingLastPacketTimeNs_ = receiveTimeNs;

    if (fecPacket)
    {
        const uint32_t groupIndex = header.packetIndex;
        if (groupIndex >= static_cast<uint32_t>(pendingFecReceived_.size()))
        {
            return completedFrames;
        }
        if (pendingFecReceived_[static_cast<int>(groupIndex)] != 0)
        {
            return completedFrames;
        }

        const std::ptrdiff_t offset = packetOffset(static_cast<uint16_t>(groupIndex));
        if (offset < 0 ||
            offset + static_cast<std::ptrdiff_t>(oxr::protocol::MAX_PACKET_PAYLOAD) >
                static_cast<std::ptrdiff_t>(pendingFecData_.size()))
        {
            return completedFrames;
        }

        std::memset(pendingFecData_.data() + offset, 0, oxr::protocol::MAX_PACKET_PAYLOAD);
        if (payload != nullptr && payloadSize > 0)
        {
            std::memcpy(pendingFecData_.data() + offset,
                        payload,
                        static_cast<size_t>(payloadSize));
        }
        pendingFecReceived_[static_cast<int>(groupIndex)] = 1;
        pendingFecGroupLastPacketSizes_[static_cast<int>(groupIndex)] =
            header.fecGroupLastPacketPayloadSize;

        if (tryFecRecovery() && isComplete())
        {
            deliveredFrameIndex_ = pendingFrameIndex_;
        deliveredPresentationTimeNs_ = pendingPresentationTimeNs_;
            completedFrames.push_back(deliverPendingFrame(receiveTimeNs));
            reset();
        }
        return completedFrames;
    }

    if (pendingPacketReceived_[header.packetIndex] != 0)
    {
        return completedFrames;
    }

    const std::ptrdiff_t offset = packetOffset(header.packetIndex);
    if (offset < 0 || offset + payloadSize > static_cast<std::ptrdiff_t>(pendingFrameData_.size()))
    {
        return completedFrames;
    }

    if (payload != nullptr && payloadSize > 0)
    {
        std::memcpy(pendingFrameData_.data() + offset,
                    payload,
                    static_cast<size_t>(payloadSize));
    }
    pendingPacketReceived_[header.packetIndex] = 1;
    pendingPacketSizes_[header.packetIndex] = static_cast<uint16_t>(payloadSize);
    ++pendingReceivedPackets_;

    if (isComplete())
    {
        deliveredFrameIndex_ = pendingFrameIndex_;
        deliveredPresentationTimeNs_ = pendingPresentationTimeNs_;
        completedFrames.push_back(deliverPendingFrame(receiveTimeNs));
        reset();
    }
    else if (tryFecRecovery() && isComplete())
    {
        deliveredFrameIndex_ = pendingFrameIndex_;
        deliveredPresentationTimeNs_ = pendingPresentationTimeNs_;
        completedFrames.push_back(deliverPendingFrame(receiveTimeNs));
        reset();
    }

    return completedFrames;
}

std::vector<AssembledVideoFrame> VideoFrameAssembler::expirePendingFrame(int64_t nowNs,
                                                                   int64_t timeoutNs)
{
    if (pendingTotalPackets_ == 0 || pendingLastPacketTimeNs_ <= 0 ||
        nowNs - pendingLastPacketTimeNs_ <= timeoutNs)
    {
        return {};
    }
    return finishPendingFrame(true, nowNs);
}

void VideoFrameAssembler::reset()
{
    pendingFrameIndex_ = UINT32_MAX;
    pendingTotalPackets_ = 0;
    pendingReceivedPackets_ = 0;
    pendingPresentationTimeNs_ = 0;
    pendingLastPacketTimeNs_ = 0;
    pendingFrameData_.clear();
    pendingPacketSizes_.clear();
    pendingPacketReceived_.clear();
    pendingFecData_.clear();
    pendingFecReceived_.clear();
    pendingFecGroupLastPacketSizes_.clear();
    pendingRecoveredWithFec_ = false;
}

bool VideoFrameAssembler::attachRenderPose(AssembledVideoFrame& frame) const
{
    for (const KeptRenderPose& kept : renderPoses_)
    {
#if defined(XRPILOT_PLANT_INDEX_ONLY)
        if (kept.frameIndex == frame.frameIndex) // planted defect for the sync trials' control
#else
        if (kept.frameIndex == frame.frameIndex && kept.presentationTimeNs == frame.presentationTimeNs)
#endif
        {
            frame.hasRenderPose = true;
            std::memcpy(frame.renderPosition, kept.position, sizeof(kept.position));
            std::memcpy(frame.renderOrientation, kept.orientation, sizeof(kept.orientation));
            return true;
        }
    }
    return false;
}

void VideoFrameAssembler::clearRenderPoses()
{
    // A new connection may come from another runtime whose clock reads lower.
    newestPresentationTimeNs_ = INT64_MIN;
    deliveredFrameIndex_ = UINT32_MAX;
    deliveredPresentationTimeNs_ = INT64_MIN;
    for (KeptRenderPose& kept : renderPoses_)
    {
        kept = {};
    }
    nextRenderPose_ = 0;
}

uint64_t VideoFrameAssembler::droppedFrames() const
{
    return droppedFrames_;
}

uint64_t VideoFrameAssembler::fecRecoveries() const
{
    return fecRecoveries_;
}

void VideoFrameAssembler::startFrame(const oxr::protocol::VideoPacketHeader& header,
                                     int64_t receiveTimeNs)
{
    pendingFrameIndex_ = header.frameIndex;
    pendingTotalPackets_ = header.totalPackets;
    pendingReceivedPackets_ = 0;
    pendingPresentationTimeNs_ = header.presentationTimeNs;
    newestPresentationTimeNs_ = std::max(newestPresentationTimeNs_, header.presentationTimeNs);
    pendingLastPacketTimeNs_ = receiveTimeNs;
    pendingRecoveredWithFec_ = false;
    pendingFrameData_.assign(size_t(pendingTotalPackets_) * oxr::protocol::MAX_PACKET_PAYLOAD, 0);
    pendingPacketSizes_.assign(pendingTotalPackets_, 0);
    pendingPacketReceived_.assign(pendingTotalPackets_, 0);

    const uint32_t fecGroups = oxr::fec::GroupCount(pendingTotalPackets_);
    pendingFecData_.assign(size_t(fecGroups) * oxr::protocol::MAX_PACKET_PAYLOAD, 0);
    pendingFecReceived_.assign(fecGroups, 0);
    pendingFecGroupLastPacketSizes_.assign(fecGroups, 0);
}

std::vector<AssembledVideoFrame> VideoFrameAssembler::finishPendingFrame(bool countDropIfIncomplete,
                                                                   int64_t receiveTimeNs)
{
    std::vector<AssembledVideoFrame> completedFrames;
    if (pendingTotalPackets_ == 0)
    {
        return completedFrames;
    }
    if (isComplete() || (tryFecRecovery() && isComplete()))
    {
        deliveredFrameIndex_ = pendingFrameIndex_;
        deliveredPresentationTimeNs_ = pendingPresentationTimeNs_;
        completedFrames.push_back(deliverPendingFrame(receiveTimeNs));
    }
    else if (countDropIfIncomplete)
    {
        ++droppedFrames_;
    }
    reset();
    return completedFrames;
}

bool VideoFrameAssembler::tryFecRecovery()
{
    if (pendingTotalPackets_ == 0 || pendingFecReceived_.empty())
    {
        return false;
    }

    bool recoveredAnyPacket = false;
    for (int groupIndex = 0; groupIndex < static_cast<int>(pendingFecReceived_.size()); ++groupIndex)
    {
        if (pendingFecReceived_[groupIndex] == 0)
        {
            continue;
        }

        uint32_t groupStart = 0;
        uint32_t groupEnd = 0;
        oxr::fec::GroupRange(static_cast<uint32_t>(groupIndex),
                             pendingTotalPackets_,
                             groupStart,
                             groupEnd);

        int missingIndex = -1;
        int missingCount = 0;
        for (uint32_t packetIndex = groupStart; packetIndex < groupEnd; ++packetIndex)
        {
            if (pendingPacketReceived_[static_cast<int>(packetIndex)] == 0)
            {
                missingIndex = static_cast<int>(packetIndex);
                ++missingCount;
            }
        }
        if (missingCount != 1)
        {
            continue;
        }

        uint8_t* destination = reinterpret_cast<uint8_t*>(
            pendingFrameData_.data() + packetOffset(static_cast<uint16_t>(missingIndex)));
        const uint8_t* fecPayload = reinterpret_cast<const uint8_t*>(
            pendingFecData_.data() + packetOffset(static_cast<uint16_t>(groupIndex)));
        std::memcpy(destination, fecPayload, oxr::protocol::MAX_PACKET_PAYLOAD);

        for (uint32_t packetIndex = groupStart; packetIndex < groupEnd; ++packetIndex)
        {
            if (static_cast<int>(packetIndex) == missingIndex)
            {
                continue;
            }
            const uint16_t packetSize =
                pendingPacketSizes_[static_cast<int>(packetIndex)];
            const uint8_t* source = reinterpret_cast<const uint8_t*>(
                pendingFrameData_.data() + packetOffset(static_cast<uint16_t>(packetIndex)));
            for (uint16_t byteIndex = 0; byteIndex < packetSize; ++byteIndex)
            {
                destination[byteIndex] ^= source[byteIndex];
            }
        }

        pendingPacketReceived_[missingIndex] = 1;
        uint16_t recoveredSize = static_cast<uint16_t>(oxr::protocol::MAX_PACKET_PAYLOAD);
        if (missingIndex == static_cast<int>(groupEnd - 1))
        {
            const uint16_t groupLastPacketSize =
                pendingFecGroupLastPacketSizes_[groupIndex];
            if (groupLastPacketSize > 0 &&
                groupLastPacketSize <= oxr::protocol::MAX_PACKET_PAYLOAD)
            {
                recoveredSize = groupLastPacketSize;
            }
        }
        pendingPacketSizes_[missingIndex] = recoveredSize;
        ++pendingReceivedPackets_;
        ++fecRecoveries_;
        pendingRecoveredWithFec_ = true;
        recoveredAnyPacket = true;
    }
    return recoveredAnyPacket;
}

bool VideoFrameAssembler::isComplete() const
{
    return pendingTotalPackets_ > 0 && pendingReceivedPackets_ == pendingTotalPackets_;
}

AssembledVideoFrame VideoFrameAssembler::deliverPendingFrame(int64_t receiveTimeNs) const
{
    std::ptrdiff_t totalSize = 0;
    for (uint16_t packetSize : pendingPacketSizes_)
    {
        totalSize += packetSize;
    }

    std::vector<uint8_t> nalUnit;
    nalUnit.resize(static_cast<size_t>(totalSize));
    std::ptrdiff_t destinationOffset = 0;
    for (int i = 0; i < static_cast<int>(pendingPacketSizes_.size()); ++i)
    {
        const std::ptrdiff_t packetSize = pendingPacketSizes_[i];
        if (packetSize <= 0)
        {
            continue;
        }
        const std::ptrdiff_t sourceOffset = packetOffset(static_cast<uint16_t>(i));
        std::memcpy(nalUnit.data() + destinationOffset,
                    pendingFrameData_.data() + sourceOffset,
                    static_cast<size_t>(packetSize));
        destinationOffset += packetSize;
    }

    AssembledVideoFrame frame;
    frame.nalUnit = std::move(nalUnit);
    frame.frameIndex = pendingFrameIndex_;
    frame.presentationTimeNs = pendingPresentationTimeNs_;
    frame.receiveTimeNs = receiveTimeNs;
    frame.recoveredWithFec = pendingRecoveredWithFec_;
    attachRenderPose(frame);
    return frame;
}
