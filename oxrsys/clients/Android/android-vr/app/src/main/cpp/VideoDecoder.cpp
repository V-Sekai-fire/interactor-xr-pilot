// SPDX-License-Identifier: MPL-2.0

#include <volk.h>
#include <pyrowave.h>

#include "VideoDecoder.h"

#include <android/log.h>

#include <chrono>
#include <cstring>
#include <utility>

#define LOG_TAG "OXRSys-Decoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace oxr
{

namespace
{

constexpr uint64_t FenceTimeoutNs = 1'000'000'000ull;

int64_t SteadyClockNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t PlaneWidth(uint32_t plane, uint32_t width)
{
    return plane == 0 ? width : width / 2;
}

uint32_t PlaneHeight(uint32_t plane, uint32_t height)
{
    return plane == 0 ? height : height / 2;
}

} // namespace

struct VideoDecoder::Gpu
{
    // A plane PyroWave decodes into, as a colour attachment of the fragment path.
    struct Plane
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    // One slot's copy of the three planes, rows tightly packed, mapped for the GL thread to upload.
    struct Staging
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
    };

    // pyrowave_create_device keeps pointers into these for the device's lifetime.
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    pyrowave_device_create_queue_info pyroQueue = {};

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties = {};

    pyrowave_device pyro = nullptr;
    pyrowave_decoder decoder = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    Plane planes[PlaneCount];
    Staging staging[SlotCount];
    VkDeviceSize planeOffset[PlaneCount] = {};

    ~Gpu()
    {
        if (decoder != nullptr)
        {
            pyrowave_decoder_destroy(decoder);
        }
        if (device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device);
            for (Plane& plane : planes)
            {
                vkDestroyImage(device, plane.image, nullptr);
                vkFreeMemory(device, plane.memory, nullptr);
            }
            for (Staging& slot : staging)
            {
                vkDestroyBuffer(device, slot.buffer, nullptr);
                vkFreeMemory(device, slot.memory, nullptr); // unmaps
            }
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
        }
        if (pyro != nullptr)
        {
            pyrowave_device_destroy(pyro);
        }
        if (device != VK_NULL_HANDLE)
        {
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance, nullptr);
        }
    }

    bool CreateDevice()
    {
        if (volkInitialize() != VK_SUCCESS)
        {
            LOGE("No Vulkan loader");
            return false;
        }
        appInfo.pApplicationName = "OXRSys PyroWave decoder";
        appInfo.apiVersion = VK_API_VERSION_1_3;
        instanceInfo.pApplicationInfo = &appInfo;
        if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
        {
            LOGE("vkCreateInstance failed");
            return false;
        }
        volkLoadInstanceOnly(instance);

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        for (VkPhysicalDevice candidate : devices)
        {
            VkPhysicalDeviceProperties properties = {};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            LOGI("Vulkan device %s, API %u.%u", properties.deviceName,
                 VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion));
            if (properties.apiVersion >= VK_API_VERSION_1_3)
            {
                physical = candidate;
                break;
            }
        }
        if (physical == VK_NULL_HANDLE)
        {
            LOGE("PyroWave needs a Vulkan 1.3 device; none found");
            return false;
        }

        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        while (family < count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
        {
            ++family;
        }
        if (family == count)
        {
            LOGE("No graphics queue");
            return false;
        }

        // Every supported core feature is enabled, which covers PyroWave's subgroup needs.
        features.pNext = &features11;
        features11.pNext = &features12;
        features12.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        features.features.robustBufferAccess = VK_FALSE;

        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        deviceInfo.pNext = &features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS)
        {
            LOGE("vkCreateDevice failed");
            return false;
        }
        volkLoadDevice(device);
        vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);

        pyroQueue = {queue, family, 0};
        pyrowave_device_create_info info = {};
        info.GetInstanceProcAddr = vkGetInstanceProcAddr;
        info.instance = instance;
        info.physical_device = physical;
        info.device = device;
        info.instance_create_info = &instanceInfo;
        info.device_create_info = &deviceInfo;
        info.queue_info = &pyroQueue;
        info.queue_info_count = 1;
        if (pyrowave_create_device(&info, &pyro) != PYROWAVE_SUCCESS)
        {
            LOGE("pyrowave_create_device failed");
            return false;
        }
        pyrowave_device_set_queue_type(pyro, VK_QUEUE_GRAPHICS_BIT);

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
        {
            return false;
        }
        VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdInfo.commandPool = pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        return vkAllocateCommandBuffers(device, &cmdInfo, &cmd) == VK_SUCCESS;
    }

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const
    {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) != 0 && (memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool CreatePlane(Plane& plane, uint32_t w, uint32_t h)
    {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8_UNORM;
        imageInfo.extent = {w, h, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &imageInfo, nullptr, &plane.image) != VK_SUCCESS)
        {
            LOGE("vkCreateImage R8 %ux%u failed", w, h);
            return false;
        }
        VkMemoryRequirements requirements = {};
        vkGetImageMemoryRequirements(device, plane.image, &requirements);
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(device, &allocInfo, nullptr, &plane.memory) != VK_SUCCESS ||
            vkBindImageMemory(device, plane.image, plane.memory, 0) != VK_SUCCESS)
        {
            LOGE("Allocating an R8 %ux%u plane failed", w, h);
            return false;
        }
        return true;
    }

    // Host-coherent memory, cached where the device offers it, since the GL thread reads every byte.
    bool CreateStaging(Staging& slot, VkDeviceSize size)
    {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufferInfo, nullptr, &slot.buffer) != VK_SUCCESS)
        {
            return false;
        }
        VkMemoryRequirements requirements = {};
        vkGetBufferMemoryRequirements(device, slot.buffer, &requirements);
        const VkMemoryPropertyFlags coherent =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = MemoryType(requirements.memoryTypeBits, coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type == UINT32_MAX)
        {
            type = MemoryType(requirements.memoryTypeBits, coherent);
        }
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = type;
        void* mapped = nullptr;
        if (type == UINT32_MAX || vkAllocateMemory(device, &allocInfo, nullptr, &slot.memory) != VK_SUCCESS ||
            vkBindBufferMemory(device, slot.buffer, slot.memory, 0) != VK_SUCCESS ||
            vkMapMemory(device, slot.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        {
            LOGE("Allocating a %llu-byte plane staging buffer failed", static_cast<unsigned long long>(size));
            return false;
        }
        slot.mapped = static_cast<uint8_t*>(mapped);
        return true;
    }

    bool CreateFrames(uint32_t w, uint32_t h)
    {
        width = w;
        height = h;
        pyrowave_decoder_create_info info = {};
        info.device = pyro;
        info.width = static_cast<int>(w);
        info.height = static_cast<int>(h);
        info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
        // Headset GPUs are tilers; the fragment path also needs only colour-attachment planes, which
        // every GPU supports for R8, where R8 storage images may not be.
        info.fragment_path = true;
        LOGI("PyroWave fragment path: used; device prefers it: %s",
             pyrowave_decoder_device_prefers_fragment_path(pyro) ? "yes" : "no");
        if (pyrowave_decoder_create(&info, &decoder) != PYROWAVE_SUCCESS)
        {
            LOGE("pyrowave_decoder_create %ux%u failed", w, h);
            return false;
        }
        VkDeviceSize size = 0;
        for (uint32_t i = 0; i < PlaneCount; ++i)
        {
            if (!CreatePlane(planes[i], PlaneWidth(i, w), PlaneHeight(i, h)))
            {
                return false;
            }
            planeOffset[i] = size;
            size += VkDeviceSize(PlaneWidth(i, w)) * PlaneHeight(i, h);
        }
        for (Staging& slot : staging)
        {
            if (!CreateStaging(slot, size))
            {
                return false;
            }
        }
        return true;
    }

    void Barrier(VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                 VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier barriers[PlaneCount] = {};
        for (uint32_t i = 0; i < PlaneCount; ++i)
        {
            VkImageMemoryBarrier& b = barriers[i];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = planes[i].image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, PlaneCount, barriers);
    }

    // Decodes, copies the planes into one slot's staging buffer and returns once the copy has landed.
    bool Decode(const std::vector<uint8_t>& data, int slot)
    {
        pyrowave_decoder_clear(decoder);
        if (pyrowave_decoder_push_packet(decoder, data.data(), data.size()) != PYROWAVE_SUCCESS ||
            !pyrowave_decoder_decode_is_ready(decoder, false))
        {
            return false;
        }

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS || vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
        {
            return false;
        }
        // The previous contents are discarded; the last frame's copy finished before this submit.
        Barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

        pyrowave_gpu_buffers views = {};
        for (uint32_t i = 0; i < PlaneCount; ++i)
        {
            views.planes[i].image = planes[i].image;
            views.planes[i].width = PlaneWidth(i, width);
            views.planes[i].height = PlaneHeight(i, height);
            views.planes[i].image_format = VK_FORMAT_R8_UNORM;
            views.planes[i].view_format = VK_FORMAT_R8_UNORM;
            views.planes[i].aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            views.planes[i].swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
            views.planes[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        pyrowave_device_set_command_buffer(pyro, cmd);
        const pyrowave_result decoded = pyrowave_decoder_decode_gpu_buffer(decoder, nullptr, nullptr, &views);
        pyrowave_device_set_command_buffer(pyro, VK_NULL_HANDLE);

        Barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (uint32_t i = 0; i < PlaneCount; ++i)
        {
            VkBufferImageCopy region = {};
            region.bufferOffset = planeOffset[i];
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {PlaneWidth(i, width), PlaneHeight(i, height), 1};
            vkCmdCopyImageToBuffer(cmd, planes[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging[slot].buffer, 1,
                                   &region);
        }
        VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr,
                             0, nullptr);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS || decoded != PYROWAVE_SUCCESS)
        {
            return false;
        }

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        return vkResetFences(device, 1, &fence) == VK_SUCCESS &&
               vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS &&
               vkWaitForFences(device, 1, &fence, VK_TRUE, FenceTimeoutNs) == VK_SUCCESS;
    }
};

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder()
{
    Shutdown();
}

bool VideoDecoder::FrameSize(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height)
{
    if (data == nullptr || size < 8)
    {
        return false;
    }
    uint32_t word = 0;
    std::memcpy(&word, data, sizeof(word));
    if ((word >> 31) != 1)
    {
        return false;
    }
    *width = (word & 0x3FFF) + 1;
    *height = ((word >> 14) & 0x3FFF) + 1;
    return true;
}

bool VideoDecoder::Initialize(uint32_t width, uint32_t height)
{
    Shutdown();
    if (width == 0 || height == 0 || (width % 2) != 0 || (height % 2) != 0)
    {
        LOGE("PyroWave 4:2:0 needs an even, non-zero size; got %ux%u", width, height);
        return false;
    }

    std::unique_ptr<Gpu> gpu = std::make_unique<Gpu>();
    if (!gpu->CreateDevice() || !gpu->CreateFrames(width, height))
    {
        return false;
    }
    gpu_ = std::move(gpu);
    width_ = width;
    height_ = height;
    heldSlot_ = -1;
    readySlot_ = -1;
    hasPending_ = false;
    pendingReplaced_ = 0;
    running_.store(true);
    decodeThread_ = std::thread(&VideoDecoder::DecodeThreadMain, this);
    LOGI("PyroWave decoder initialized: %ux%u", width, height);
    return true;
}

void VideoDecoder::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.store(false);
    }
    wake_.notify_all();
    if (decodeThread_.joinable())
    {
        decodeThread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    gpu_.reset();
    heldSlot_ = -1;
    readySlot_ = -1;
    hasPending_ = false;
    pending_.clear();
}

bool VideoDecoder::SubmitFrame(const uint8_t* data, size_t size, int64_t presentationTimeUs,
                               int64_t receiveTimeNs, bool alphaBlend)
{
    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;
    if (!FrameSize(data, size, &frameWidth, &frameHeight))
    {
        return false;
    }
    if (frameWidth != width_ || frameHeight != height_)
    {
        LOGW("Dropped a %ux%u PyroWave frame; the decoder is %ux%u", frameWidth, frameHeight, width_, height_);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load())
        {
            return false;
        }
        if (hasPending_)
        {
            ++pendingReplaced_;
        }
        pending_.assign(data, data + size);
        pendingMetadata_ = {presentationTimeUs, receiveTimeNs, SteadyClockNowNs(), alphaBlend};
        hasPending_ = true;
    }
    wake_.notify_one();
    return true;
}

void VideoDecoder::DecodeThreadMain()
{
    LOGI("Decode thread started");
    std::vector<uint8_t> data;
    uint32_t failures = 0;
    for (;;)
    {
        FrameMetadata metadata;
        uint32_t replaced = 0;
        int slot = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return !running_.load() || hasPending_; });
            if (!running_.load())
            {
                break;
            }
            std::swap(data, pending_);
            metadata = pendingMetadata_;
            replaced = pendingReplaced_;
            hasPending_ = false;
            pendingReplaced_ = 0;
            while (slot == heldSlot_ || slot == readySlot_)
            {
                ++slot;
            }
        }

        if (!gpu_->Decode(data, slot))
        {
            if (++failures <= 5 || failures % 300 == 0)
            {
                LOGE("PyroWave decode failed (%u so far)", failures);
            }
            continue;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        slots_[slot].metadata = metadata;
        slots_[slot].skippedBefore = replaced;
        if (readySlot_ >= 0)
        {
            slots_[slot].skippedBefore += 1 + slots_[readySlot_].skippedBefore;
        }
        readySlot_ = slot;
    }
    LOGI("Decode thread ended");
}

bool VideoDecoder::AcquireFrame(DecodedFrame* outFrame)
{
    if (outFrame == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load() || readySlot_ < 0)
    {
        return false;
    }
    // The renderer has uploaded the previously held planes, so their slot is free again.
    heldSlot_ = readySlot_;
    readySlot_ = -1;

    const Slot& slot = slots_[heldSlot_];
    for (uint32_t i = 0; i < PlaneCount; ++i)
    {
        outFrame->planes[i] = gpu_->staging[heldSlot_].mapped + gpu_->planeOffset[i];
        outFrame->planeWidth[i] = PlaneWidth(i, width_);
        outFrame->planeHeight[i] = PlaneHeight(i, height_);
    }
    outFrame->presentationTimeUs = slot.metadata.presentationTimeUs;
    outFrame->bufferWidth = width_;
    outFrame->bufferHeight = height_;
    outFrame->bufferStride = width_;
    outFrame->cropLeft = 0;
    outFrame->cropTop = 0;
    outFrame->cropRight = static_cast<int32_t>(width_);
    outFrame->cropBottom = static_cast<int32_t>(height_);
    outFrame->localReceiveTimeNs = slot.metadata.receiveTimeNs;
    outFrame->localSubmitTimeNs = slot.metadata.submitTimeNs;
    outFrame->localAcquireTimeNs = SteadyClockNowNs();
    outFrame->skippedFramesBeforeAcquire = slot.skippedBefore;
    outFrame->alphaBlend = slot.metadata.alphaBlend;
    skippedFramesBeforeAcquire_.fetch_add(slot.skippedBefore);
    return true;
}

} // namespace oxr
