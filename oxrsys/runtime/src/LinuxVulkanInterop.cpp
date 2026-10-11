// SPDX-License-Identifier: MPL-2.0

#include "LinuxVulkanInterop.h"
#include "VulkanDispatch.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>

const char* const kLinuxVulkanInstanceExtensions =
    "VK_KHR_external_memory_capabilities VK_KHR_external_semaphore_capabilities "
    "VK_KHR_get_physical_device_properties2";
const char* const kLinuxVulkanDeviceExtensions =
    "VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2 VK_KHR_external_memory "
    "VK_KHR_external_memory_fd VK_KHR_timeline_semaphore VK_KHR_external_semaphore "
    "VK_KHR_external_semaphore_fd";

namespace
{

constexpr uint64_t WaitTimeoutNs = 1'000'000'000;

// One packed image being copied, one being encoded, one spare.
constexpr size_t SlotCount = 3;

std::atomic<uint64_t> gNextId{1};

struct DeviceFuncs
{
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;
    PFN_vkWaitSemaphores waitSemaphores = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;

    bool Load(VkDevice device)
    {
        PFN_vkGetDeviceProcAddr get = gVulkanDispatch.getDeviceProcAddr;
        if (get == nullptr)
        {
            return false;
        }
        // Timeline semaphores are core in Vulkan 1.2 and an extension before it.
        waitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(get(device, "vkWaitSemaphores"));
        if (waitSemaphores == nullptr)
        {
            waitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(get(device, "vkWaitSemaphoresKHR"));
        }
        if (waitSemaphores == nullptr)
        {
            spdlog::error("OXRSys: Vulkan interop is missing vkWaitSemaphores");
            return false;
        }
#define OXR_LOAD(member, name)                                                                                         \
    member = reinterpret_cast<decltype(member)>(get(device, name));                                                     \
    if (member == nullptr)                                                                                             \
    {                                                                                                                  \
        spdlog::error("OXRSys: Vulkan interop is missing {}", name);                                                   \
        return false;                                                                                                  \
    }
        OXR_LOAD(getDeviceQueue, "vkGetDeviceQueue");
        OXR_LOAD(createImage, "vkCreateImage");
        OXR_LOAD(destroyImage, "vkDestroyImage");
        OXR_LOAD(getImageMemoryRequirements, "vkGetImageMemoryRequirements");
        OXR_LOAD(allocateMemory, "vkAllocateMemory");
        OXR_LOAD(freeMemory, "vkFreeMemory");
        OXR_LOAD(bindImageMemory, "vkBindImageMemory");
        OXR_LOAD(getMemoryFd, "vkGetMemoryFdKHR");
        OXR_LOAD(createSemaphore, "vkCreateSemaphore");
        OXR_LOAD(destroySemaphore, "vkDestroySemaphore");
        OXR_LOAD(getSemaphoreFd, "vkGetSemaphoreFdKHR");
        OXR_LOAD(createCommandPool, "vkCreateCommandPool");
        OXR_LOAD(destroyCommandPool, "vkDestroyCommandPool");
        OXR_LOAD(allocateCommandBuffers, "vkAllocateCommandBuffers");
        OXR_LOAD(resetCommandBuffer, "vkResetCommandBuffer");
        OXR_LOAD(beginCommandBuffer, "vkBeginCommandBuffer");
        OXR_LOAD(endCommandBuffer, "vkEndCommandBuffer");
        OXR_LOAD(cmdPipelineBarrier, "vkCmdPipelineBarrier");
        OXR_LOAD(cmdCopyImage, "vkCmdCopyImage");
        OXR_LOAD(queueSubmit, "vkQueueSubmit");
#undef OXR_LOAD
        return true;
    }
};

// The device and the timeline semaphore, shared by the interop and every packed image it leased.
struct Core
{
    DeviceFuncs vk;
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t semaphoreId = gNextId.fetch_add(1);
    std::atomic<uint64_t> lastValue{0};

    // Blocks until the app's queue has signalled value; true when it has.
    bool Wait(uint64_t value) const
    {
        if (value == 0)
        {
            return true;
        }
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pSemaphores = &timeline;
        info.pValues = &value;
        return vk.waitSemaphores(device, &info, WaitTimeoutNs) == VK_SUCCESS;
    }

    int ExportSemaphore() const
    {
        VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        info.semaphore = timeline;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        return vk.getSemaphoreFd(device, &info, &fd) == VK_SUCCESS ? fd : -1;
    }

    ~Core()
    {
        if (timeline != VK_NULL_HANDLE)
        {
            Wait(lastValue.load());
            vk.destroySemaphore(device, timeline, nullptr);
        }
    }
};

struct PackedImage
{
    std::shared_ptr<Core> core;
    VkImageCreateInfo info = {};
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t id = gNextId.fetch_add(1);
    uint64_t lastValue = 0; // the copy that last wrote it

    int ExportMemory() const
    {
        VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        fdInfo.memory = memory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        return core->vk.getMemoryFd(core->device, &fdInfo, &fd) == VK_SUCCESS ? fd : -1;
    }

    ~PackedImage()
    {
        // A dropped frame may still have its copy in flight.
        core->Wait(lastValue);
        if (image != VK_NULL_HANDLE)
        {
            core->vk.destroyImage(core->device, image, nullptr);
        }
        if (memory != VK_NULL_HANDLE)
        {
            core->vk.freeMemory(core->device, memory, nullptr);
        }
    }
};

struct Slot
{
    std::shared_ptr<PackedImage> packed;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    uint64_t commandsValue = 0; // the submission that last used commands
    std::shared_ptr<std::atomic_bool> inUse = std::make_shared<std::atomic_bool>(false);
};

struct Interop
{
    std::shared_ptr<Core> core;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<Slot, SlotCount> slots;
    uint64_t nextValue = 0;
    bool loggedFormat = false;

    ~Interop()
    {
        for (Slot& slot : slots)
        {
            core->Wait(slot.commandsValue);
        }
        if (pool != VK_NULL_HANDLE)
        {
            core->vk.destroyCommandPool(core->device, pool, nullptr);
        }
    }

    uint32_t DeviceLocalMemoryType(uint32_t bits) const
    {
        VkPhysicalDeviceMemoryProperties properties = {};
        gVulkanDispatch.getPhysicalDeviceMemoryProperties(physical, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) != 0 &&
                (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    // An exportable image on the app's device; PyroWave imports it with a dedicated allocation,
    // so it is exported with one too.
    std::shared_ptr<PackedImage> MakePacked(VkFormat format, uint32_t width, uint32_t height)
    {
        auto packed = std::make_shared<PackedImage>();
        packed->core = core;
        VkImageCreateInfo& info = packed->info;
        info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo exported = info;
        exported.pNext = &external;
        const DeviceFuncs& vk = core->vk;
        if (vk.createImage(core->device, &exported, nullptr, &packed->image) != VK_SUCCESS)
        {
            spdlog::error("OXRSys: packed eye image creation failed ({}x{})", width, height);
            return {};
        }

        VkMemoryRequirements requirements = {};
        vk.getImageMemoryRequirements(core->device, packed->image, &requirements);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = packed->image;
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.pNext = &dedicated;
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.pNext = &exportInfo;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = DeviceLocalMemoryType(requirements.memoryTypeBits);
        if (allocInfo.memoryTypeIndex == UINT32_MAX ||
            vk.allocateMemory(core->device, &allocInfo, nullptr, &packed->memory) != VK_SUCCESS ||
            vk.bindImageMemory(core->device, packed->image, packed->memory, 0) != VK_SUCCESS)
        {
            spdlog::error("OXRSys: packed eye memory allocation failed ({}x{})", width, height);
            return {};
        }
        return packed;
    }

    void Record(VkCommandBuffer commands, VkImage packed, const LinuxEyeSlice& left, const LinuxEyeSlice& right)
    {
        const DeviceFuncs& vk = core->vk;
        const LinuxEyeSlice* eyes[2] = {&left, &right};

        // One barrier per eye subresource: both eyes may be rects of one slice.
        const bool shared = right.image == left.image && right.arrayIndex == left.arrayIndex;
        const uint32_t count = shared ? 2 : 3;

        // The packed image is overwritten whole, so its old contents are discarded.
        std::array<VkImageMemoryBarrier, 3> before = {};
        for (VkImageMemoryBarrier& barrier : before)
        {
            barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        }
        before[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        before[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before[0].image = packed;
        before[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // Each eye was released in COLOR_ATTACHMENT_OPTIMAL after its rendering on this queue.
        for (int e = 0; e < 2; ++e)
        {
            VkImageMemoryBarrier& barrier = before[1 + e];
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.image = eyes[e]->image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eyes[e]->arrayIndex, 1};
        }
        vk.cmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, count, before.data());

        // The copy reinterprets sRGB eyes as UNORM, so their encoded bytes arrive unchanged.
        int32_t x = 0;
        for (const LinuxEyeSlice* eye : eyes)
        {
            VkImageCopy region = {};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye->arrayIndex, 1};
            region.srcOffset = {eye->x, eye->y, 0};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstOffset = {x, 0, 0};
            region.extent = {eye->width, eye->height, 1};
            vk.cmdCopyImage(commands, eye->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, packed,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            x += int32_t(eye->width);
        }

        // The eyes go back as the app left them; the packed image goes to PyroWave's device in GENERAL.
        std::array<VkImageMemoryBarrier, 3> after = before;
        after[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after[0].dstAccessMask = 0;
        after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        after[0].srcQueueFamilyIndex = queueFamily;
        after[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        for (int e = 0; e < 2; ++e)
        {
            VkImageMemoryBarrier& barrier = after[1 + e];
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        vk.cmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                              0, nullptr, 0, nullptr, count, after.data());
    }
};

// The UNORM format a packed image takes for an eye format; UNDEFINED when the encoder does not take it.
VkFormat PackedFormat(VkFormat eye)
{
    switch (eye)
    {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED; // FP16 needs a tone-mapping pass, and depth is never shown
    }
}

} // namespace

std::shared_ptr<void> LinuxCreateVulkanInterop(const VulkanGraphicsContext& context)
{
    auto core = std::make_shared<Core>();
    core->device = static_cast<VkDevice>(context.device);
    if (core->device == VK_NULL_HANDLE || !core->vk.Load(core->device) ||
        gVulkanDispatch.getPhysicalDeviceMemoryProperties == nullptr)
    {
        spdlog::error("OXRSys: Vulkan interop unavailable; did the app enable {}?", kLinuxVulkanDeviceExtensions);
        return {};
    }

    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.pNext = &type;
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreInfo.pNext = &exportInfo;
    if (core->vk.createSemaphore(core->device, &semaphoreInfo, nullptr, &core->timeline) != VK_SUCCESS)
    {
        spdlog::error("OXRSys: exportable timeline semaphore creation failed");
        return {};
    }

    auto interop = std::make_shared<Interop>();
    interop->core = core;
    interop->physical = static_cast<VkPhysicalDevice>(context.physicalDevice);
    interop->queueFamily = context.queueFamilyIndex;
    core->vk.getDeviceQueue(core->device, context.queueFamilyIndex, context.queueIndex, &interop->queue);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context.queueFamilyIndex;
    if (interop->queue == VK_NULL_HANDLE ||
        core->vk.createCommandPool(core->device, &poolInfo, nullptr, &interop->pool) != VK_SUCCESS)
    {
        spdlog::error("OXRSys: Vulkan interop command pool creation failed");
        return {};
    }
    for (Slot& slot : interop->slots)
    {
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = interop->pool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        if (core->vk.allocateCommandBuffers(core->device, &commandInfo, &slot.commands) != VK_SUCCESS)
        {
            spdlog::error("OXRSys: Vulkan interop command buffer allocation failed");
            return {};
        }
    }
    spdlog::info("OXRSys: Vulkan interop ready; eyes stream from the GPU");
    return interop;
}

void LinuxPackVulkanFrame(const std::shared_ptr<void>& opaqueInterop, FrameSource& frameSource)
{
    auto interop = std::static_pointer_cast<Interop>(opaqueInterop);
    const auto* left = static_cast<const LinuxEyeSlice*>(frameSource.left.GetImage());
    const auto* right = static_cast<const LinuxEyeSlice*>(frameSource.right.GetImage());
    if (!interop || left == nullptr || right == nullptr)
    {
        frameSource.left.Reset();
        frameSource.right.Reset();
        return;
    }
    const VkFormat format = PackedFormat(left->format);
    if (format == VK_FORMAT_UNDEFINED || PackedFormat(right->format) != format)
    {
        if (!interop->loggedFormat)
        {
            spdlog::warn("OXRSys: eye formats {} and {} are not streamed", int(left->format), int(right->format));
            interop->loggedFormat = true;
        }
        frameSource.left.Reset();
        frameSource.right.Reset();
        return;
    }
    const uint32_t width = left->width + right->width;
    const uint32_t height = std::max(left->height, right->height);

    Slot* slot = nullptr;
    for (Slot& candidate : interop->slots)
    {
        bool expected = false;
        if (candidate.inUse->compare_exchange_strong(expected, true))
        {
            slot = &candidate;
            break;
        }
    }
    auto skip = [&]() {
        if (slot != nullptr)
        {
            slot->inUse->store(false);
        }
        frameSource.left.Reset();
        frameSource.right.Reset();
    };
    if (slot == nullptr)
    {
        return skip(); // the encoder still holds every packed image; skip this frame
    }

    Core& core = *interop->core;
    if (!slot->packed || slot->packed->info.format != format || slot->packed->info.extent.width != width ||
        slot->packed->info.extent.height != height)
    {
        slot->packed = interop->MakePacked(format, width, height);
        if (!slot->packed)
        {
            return skip();
        }
    }
    if (!core.Wait(slot->commandsValue) || core.vk.resetCommandBuffer(slot->commands, 0) != VK_SUCCESS)
    {
        return skip();
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (core.vk.beginCommandBuffer(slot->commands, &begin) != VK_SUCCESS)
    {
        return skip();
    }
    interop->Record(slot->commands, slot->packed->image, *left, *right);
    if (core.vk.endCommandBuffer(slot->commands) != VK_SUCCESS)
    {
        return skip();
    }

    const uint64_t value = interop->nextValue + 1;
    VkTimelineSemaphoreSubmitInfo timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &value;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &timelineInfo;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot->commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &core.timeline;
    if (core.vk.queueSubmit(interop->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        spdlog::warn("OXRSys: packed eye copy submit failed");
        return skip();
    }
    interop->nextValue = value;
    core.lastValue.store(value);
    slot->commandsValue = value;
    slot->packed->lastValue = value;

    auto* image = new LinuxPackedImage{};
    std::shared_ptr<PackedImage> packed = slot->packed;
    image->imageId = packed->id;
    image->createInfo = packed->info;
    image->exportMemory = [packed]() { return packed->ExportMemory(); };
    std::shared_ptr<Core> sharedCore = interop->core;
    image->semaphoreId = sharedCore->semaphoreId;
    image->exportSemaphore = [sharedCore]() { return sharedCore->ExportSemaphore(); };
    image->waitValue = value;

    FrameImageSource source = {};
    source.api = GraphicsApi::Vulkan;
    std::shared_ptr<std::atomic_bool> inUse = slot->inUse;
    source.image = std::shared_ptr<void>(image, [inUse](void* ptr) {
        delete static_cast<LinuxPackedImage*>(ptr);
        inUse->store(false);
    });
    // Both eyes share the packed image; the encoder reads it once.
    frameSource.left = source;
    frameSource.right = std::move(source);
}
