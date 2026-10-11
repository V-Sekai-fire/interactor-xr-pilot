// SPDX-License-Identifier: MPL-2.0
//
// Linux streaming encoder: the Vulkan interop (LinuxVulkanInterop.cpp) packs the eyes side by side
// into an image on the app's device. PyroWave imports its memory and the interop's timeline
// semaphore as opaque file descriptors on its own Vulkan device on the same GPU, and encodes
// after the copy, so the frame never leaves the GPU.

#include "Config.h"
#include "LinuxVulkanInterop.h"
#include "VideoEncoder.h"
#include "VulkanDispatch.h"

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

constexpr size_t PacketBoundary = 64 * 1024;

// The interop keeps three packed images; a few more covers one being replaced on a resize.
constexpr size_t ImportedImageLimit = 6;

struct ImportedImage
{
    uint64_t id = 0;
    pyrowave_image image = nullptr;
    pyrowave_image_view view = {};
};

struct LinuxPyroWaveState
{
    pyrowave_device device = nullptr;
    pyrowave_encoder encoder = nullptr;

    uint64_t semaphoreId = 0;
    pyrowave_sync_object sync = nullptr;
    std::deque<ImportedImage> images; // newest first

    std::vector<pyrowave_packet> packets;
    std::vector<uint8_t> bitstream;

    ~LinuxPyroWaveState()
    {
        for (ImportedImage& imported : images)
            pyrowave_image_destroy(imported.image);
        if (sync != nullptr)
            pyrowave_sync_object_destroy(sync);
        if (encoder != nullptr)
            pyrowave_encoder_destroy(encoder);
        if (device != nullptr)
            pyrowave_device_destroy(device);
    }

    // The interop's timeline semaphore, imported once per interop.
    bool EnsureSync(const LinuxPackedImage& packed)
    {
        if (sync != nullptr && semaphoreId == packed.semaphoreId)
            return true;
        if (sync != nullptr)
        {
            pyrowave_sync_object_destroy(sync);
            sync = nullptr;
        }
        const int fd = packed.exportSemaphore();
        if (fd < 0)
        {
            spdlog::error("PyroWave: exporting the interop semaphore failed");
            return false;
        }
        pyrowave_sync_object_create_info info = {};
        info.device = device;
        info.external_handle = static_cast<pyrowave_os_handle>(fd);
        info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
        if (pyrowave_sync_object_create(&info, &sync) != PYROWAVE_SUCCESS)
        {
            close(fd); // owned by PyroWave only once imported
            sync = nullptr;
            spdlog::error("PyroWave: importing the interop semaphore failed");
            return false;
        }
        semaphoreId = packed.semaphoreId;
        return true;
    }

    // Each packed image, imported once.
    const ImportedImage* EnsureImage(const LinuxPackedImage& packed)
    {
        for (const ImportedImage& imported : images)
        {
            if (imported.id == packed.imageId)
                return &imported;
        }
        const int fd = packed.exportMemory();
        if (fd < 0)
        {
            spdlog::error("PyroWave: exporting the packed eye memory failed");
            return nullptr;
        }
        ImportedImage imported;
        imported.id = packed.imageId;
        pyrowave_image_create_info info = {};
        info.device = device;
        info.external_handle = static_cast<pyrowave_os_handle>(fd);
        info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        info.image_create_info = &packed.createInfo;
        if (pyrowave_image_create(&info, &imported.image) != PYROWAVE_SUCCESS)
        {
            close(fd);
            spdlog::error("PyroWave: importing the packed eyes ({}x{}) failed", packed.createInfo.extent.width,
                          packed.createInfo.extent.height);
            return nullptr;
        }
        if (pyrowave_image_get_image_view(imported.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
                                          &imported.view) != PYROWAVE_SUCCESS)
        {
            pyrowave_image_destroy(imported.image);
            spdlog::error("PyroWave: no view of the packed eyes");
            return nullptr;
        }
        images.push_front(imported);
        while (images.size() > ImportedImageLimit)
        {
            pyrowave_image_destroy(images.back().image);
            images.pop_back();
        }
        return &images.front();
    }
};

LinuxPyroWaveState* State(void* opaque)
{
    return static_cast<LinuxPyroWaveState*>(opaque);
}

// The app's GPU, by UUID, so PyroWave can import memory exported from the app's device.
bool AppDeviceUuids(const VulkanGraphicsContext& context, pyrowave_uuid& device, pyrowave_uuid& driver)
{
    auto instance = static_cast<VkInstance>(context.instance);
    auto physical = static_cast<VkPhysicalDevice>(context.physicalDevice);
    if (gVulkanDispatch.getInstanceProcAddr == nullptr || instance == VK_NULL_HANDLE || physical == VK_NULL_HANDLE)
        return false;
    auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        gVulkanDispatch.getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    if (getProperties2 == nullptr)
    {
        getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            gVulkanDispatch.getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
    }
    if (getProperties2 == nullptr)
        return false;
    VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &ids;
    getProperties2(physical, &properties);
    std::memcpy(device.uuid, ids.deviceUUID, VK_UUID_SIZE);
    std::memcpy(driver.uuid, ids.driverUUID, VK_UUID_SIZE);
    return true;
}

} // namespace

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder()
{
    Shutdown();
}

bool VideoEncoder::SupportsFoveatedEncoding(const GraphicsContext& /*graphicsContext*/)
{
    return false;
}

oxr::protocol::VideoCodec VideoEncoder::StreamCodec()
{
    return oxr::protocol::VideoCodec::PyroWave;
}

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateMbps,
                              const GraphicsContext& graphicsContext)
{
    Shutdown();

    // 4:2:0 and the side-by-side split both want even sizes.
    width_ = width & ~3u;
    height_ = height & ~1u;
    eyeWidth_ = width_ / 2;
    fps_ = std::max(fps, 1u);
    bitrateMbps_ = bitrateMbps;
    graphicsContext_ = graphicsContext;
    frameCount_ = 0;
    shuttingDown_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);

    if (graphicsContext.api != GraphicsApi::Vulkan)
    {
        return false;
    }

    auto state = std::make_unique<LinuxPyroWaveState>();
    // Encode on the app's GPU, which the packed eyes' memory lives on.
    pyrowave_uuid deviceUuid = {};
    pyrowave_uuid driverUuid = {};
    if (!AppDeviceUuids(graphicsContext.vulkan, deviceUuid, driverUuid) ||
        pyrowave_create_device_by_compat(0, 0, &deviceUuid, &driverUuid, nullptr, &state->device) != PYROWAVE_SUCCESS)
    {
        spdlog::error("PyroWave: no Vulkan device on the app's GPU");
        return false;
    }
    pyrowave_encoder_create_info info = {};
    info.device = state->device;
    info.width = static_cast<int>(width_);
    info.height = static_cast<int>(height_);
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    if (pyrowave_encoder_create(&info, &state->encoder) != PYROWAVE_SUCCESS)
    {
        spdlog::error("PyroWave: encoder {}x{} creation failed", width_, height_);
        return false;
    }

    linux_ = state.release();
    spdlog::info("PyroWave: {}x{} @ {}Hz {}Mbps ({} bytes per frame)", width_, height_, fps_, bitrateMbps_,
                 static_cast<size_t>(bitrateMbps_) * 1'000'000 / 8 / fps_);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);
    delete State(linux_);
    linux_ = nullptr;
    inFlightFrameCount_.store(0);
}

bool VideoEncoder::Encode(FrameImageSource imageSource, int64_t timestampNs, OnNalUnitCallback callback,
                          OnFrameEncodedCallback frameCallback)
{
    FrameSource frameSource = {};
    frameSource.left = std::move(imageSource);
    return EncodeInternal(std::move(frameSource), false, timestampNs, std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeStereo(FrameSource frameSource, int64_t timestampNs, OnNalUnitCallback callback,
                                OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(std::move(frameSource), true, timestampNs, std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeInternal(FrameSource frameSource, bool /*stereo*/, int64_t timestampNs,
                                  OnNalUnitCallback callback, OnFrameEncodedCallback frameCallback)
{
    LinuxPyroWaveState* state = State(linux_);
    if (state == nullptr)
    {
        return false;
    }

    Clock::time_point encodeStart = Clock::now();
    inFlightFrameCount_.fetch_add(1);
    VideoEncoder::FrameMetrics metrics = {};
    metrics.frameNumber = frameNumberCounter_.fetch_add(1) + 1;
    metrics.timestampNs = timestampNs;
    metrics.keyframe = true;

    auto finish = [&](bool emitted) {
        frameCount_ += emitted ? 1 : 0;
        metrics.totalLatencyMs = ToMilliseconds(Clock::now() - encodeStart);
        inFlightFrameCount_.fetch_sub(1);
        if (!emitted)
        {
            droppedFrameCount_.fetch_add(1);
            metrics.frameDropped = true;
        }
        if (frameCallback)
        {
            frameCallback(metrics);
        }
        return emitted;
    };

    // Both eyes, already packed side by side by the interop.
    const auto* packed = static_cast<const LinuxPackedImage*>(frameSource.left.GetImage());
    if (packed == nullptr || !state->EnsureSync(*packed))
    {
        return finish(false);
    }
    const ImportedImage* imported = state->EnsureImage(*packed);
    if (imported == nullptr)
    {
        return finish(false);
    }

    // The packed image arrives in GENERAL, released to the external queue family by the app's queue.
    pyrowave_gpu_external_reference reference = {imported->image, VK_QUEUE_FAMILY_EXTERNAL};
    pyrowave_gpu_sync_operation acquire = {};
    acquire.images = &reference;
    acquire.num_images = 1;
    acquire.sync = {pyrowave_sync_object_get_semaphore(state->sync), packed->waitValue};
    pyrowave_gpu_sync_operation release = {};
    release.images = &reference;
    release.num_images = 1;

    pyrowave_scaled_encode_info scaled = {};
    scaled.view = imported->view;
    scaled.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scaled.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scaled.intermediate_plane_format = VK_FORMAT_R8_UNORM;
    scaled.ycbcr_chroma_midpoint = 0.5f;

    pyrowave_rate_control rate = {};
    rate.maximum_bitstream_size = static_cast<size_t>(bitrateMbps_) * 1'000'000 / 8 / fps_;

    Clock::time_point submitStart = Clock::now();
    pyrowave_result result =
        pyrowave_encoder_encode_gpu_scaled_synchronous(state->encoder, &acquire, &release, &scaled, &rate);
    size_t packetCount = 0;
    if (result == PYROWAVE_SUCCESS)
    {
        result = pyrowave_encoder_compute_num_packets(state->encoder, PacketBoundary, &packetCount);
    }
    size_t written = 0;
    if (result == PYROWAVE_SUCCESS)
    {
        state->packets.resize(packetCount);
        state->bitstream.resize(rate.maximum_bitstream_size + PacketBoundary);
        result = pyrowave_encoder_packetize(state->encoder, state->packets.data(), PacketBoundary, &written,
                                            state->bitstream.data(), state->bitstream.size());
    }
    metrics.encodeSubmitMs = ToMilliseconds(Clock::now() - submitStart);
    if (result != PYROWAVE_SUCCESS || written == 0)
    {
        spdlog::warn("PyroWave: encode failed ({})", static_cast<int>(result));
        return finish(false);
    }

    const pyrowave_packet& last = state->packets[written - 1];
    const size_t bytes = last.offset + last.size;
    if (metrics.frameNumber < 5 || metrics.frameNumber % 600 == 0)
    {
        spdlog::info("PyroWave: frame {} {} bytes (budget {}), encode {:.2f} ms", metrics.frameNumber, bytes,
                     rate.maximum_bitstream_size, metrics.encodeSubmitMs);
    }
    if (callback)
    {
        callback(state->bitstream.data(), bytes, true, timestampNs);
    }
    return finish(true);
}

void VideoEncoder::SetBitrate(uint32_t bitrateMbps)
{
    bitrateMbps_ = bitrateMbps;
}

bool VideoEncoder::AcquireSlot(size_t& outSlotIndex)
{
    outSlotIndex = 0;
    return false;
}

void VideoEncoder::ReleaseSlot(size_t /*slotIndex*/) {}

void VideoEncoder::DestroySlots() {}
