// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace oxr
{

/**
 * PyroWave decoder on a Vulkan device of its own.
 *
 * A decode thread decodes the newest submitted frame into three R8 planes (Y, Cb, Cr) on the GPU and
 * copies them into a persistently mapped buffer, waiting for the GPU before publishing them. The
 * renderer copies the planes into R8 images on its own Vulkan device. R8 AHardwareBuffers, which sharing
 * the planes across devices directly would need, are missing on some (the Steam Frame's Android container).
 */
class VideoDecoder
{
public:
    static constexpr uint32_t PlaneCount = 3;

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool Initialize(uint32_t width, uint32_t height);
    void Shutdown();

    // Queues one PyroWave frame; a frame not yet decoded is replaced by a newer one.
    bool SubmitFrame(const uint8_t* data, size_t size, int64_t presentationTimeUs,
                     int64_t receiveTimeNs, bool alphaBlend);

    struct DecodedFrame
    {
        // Tightly packed rows of each plane, valid until the next AcquireFrame.
        const uint8_t* planes[PlaneCount] = {};
        uint32_t planeWidth[PlaneCount] = {};
        uint32_t planeHeight[PlaneCount] = {};
        int64_t presentationTimeUs = 0;
        uint32_t bufferWidth = 0;
        uint32_t bufferHeight = 0;
        uint32_t bufferStride = 0;
        int32_t cropLeft = 0;
        int32_t cropTop = 0;
        int32_t cropRight = 0;
        int32_t cropBottom = 0;
        int64_t localReceiveTimeNs = 0;
        int64_t localSubmitTimeNs = 0;
        int64_t localAcquireTimeNs = 0;
        uint32_t skippedFramesBeforeAcquire = 0;
        bool alphaBlend = false;
    };

    // The planes stay unwritten until the next successful AcquireFrame.
    bool AcquireFrame(DecodedFrame* outFrame);

    bool IsInitialized() const { return running_.load(); }

    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }
    uint32_t GetSkippedFramesBeforeAcquire() const { return skippedFramesBeforeAcquire_.load(); }

    // Width and height from a PyroWave frame's sequence header, or false without one.
    static bool FrameSize(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height);

private:
    struct Gpu;

    struct FrameMetadata
    {
        int64_t presentationTimeUs = 0;
        int64_t receiveTimeNs = 0;
        int64_t submitTimeNs = 0;
        bool alphaBlend = false;
    };

    struct Slot
    {
        FrameMetadata metadata;
        uint32_t skippedBefore = 0;
    };

    static constexpr int SlotCount = 3;

    void DecodeThreadMain();

    std::unique_ptr<Gpu> gpu_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::thread decodeThread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<uint8_t> pending_;
    FrameMetadata pendingMetadata_;
    bool hasPending_ = false;
    uint32_t pendingReplaced_ = 0;

    Slot slots_[SlotCount];
    int heldSlot_ = -1;   // being read by the renderer
    int readySlot_ = -1;  // decoded, not yet acquired
    std::atomic<uint32_t> skippedFramesBeforeAcquire_{0};
};

} // namespace oxr
