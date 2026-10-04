// SPDX-License-Identifier: MPL-2.0
//
// Ported from oxrsys clients/Qt/oxrsys-simulator-shared/src/PyroWaveDecoder.cpp: the decode and
// pack path is unchanged; the device now comes from the caller and the swapchain is the caller's.

#include "xrpilot/GpuDecoder.h"

#include <volk.h>
#include <pyrowave.h>

#include "yuv420_to_rgbx_spv.h"

#include <algorithm>
#include <cstring>

namespace xrpilot
{

namespace
{

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

uint32_t PlaneWidth(int plane, int width)
{
    return uint32_t(plane == 0 ? width : width / 2);
}

uint32_t PlaneHeight(int plane, int height)
{
    return uint32_t(plane == 0 ? height : height / 2);
}

} // namespace

struct GpuDecoder::Gpu
{
    GpuContext context;
    pyrowave_device pyro = nullptr;
    pyrowave_decoder decoder = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties = {};

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;

    int width = 0;
    int height = 0;
    Image planes[3];
    Buffer packed[3]; // plane bytes, four to a word, as the kernel reads them
    Buffer params;
    Buffer rgbx;
    Image frame; // RGBA8, the blit source; left in TRANSFER_SRC_OPTIMAL
    bool hasFrame = false;
    Image white; // 1x1 RGBA8 white, the reticle's blit source; TRANSFER_SRC_OPTIMAL once made
    bool whiteReady = false;

    ~Gpu()
    {
        releaseFrame();
        if (whiteReady)
        {
            releaseImage(white);
        }
        if (device != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
        }
        if (pyro != nullptr)
        {
            pyrowave_device_destroy(pyro);
        }
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
    {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) != 0 && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags, VkDeviceMemory& memory)
    {
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = memoryType(requirements.memoryTypeBits, flags);
        return info.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(device, &info, nullptr, &memory) == VK_SUCCESS;
    }

    bool makeBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS)
        {
            return false;
        }
        VkMemoryRequirements requirements = {};
        vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
        if (!allocate(requirements, flags, out.memory) ||
            vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS)
        {
            releaseBuffer(out);
            return false;
        }
        return (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ||
               vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS;
    }

    bool makeImage(Image& out, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage)
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        if (vkCreateImage(device, &info, nullptr, &out.image) != VK_SUCCESS)
        {
            return false;
        }
        VkMemoryRequirements requirements = {};
        vkGetImageMemoryRequirements(device, out.image, &requirements);
        return allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.memory) &&
               vkBindImageMemory(device, out.image, out.memory, 0) == VK_SUCCESS;
    }

    void releaseBuffer(Buffer& b)
    {
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
        b = {};
    }

    void releaseImage(Image& image)
    {
        vkDestroyImage(device, image.image, nullptr);
        vkFreeMemory(device, image.memory, nullptr);
        image = {};
    }

    void releaseFrame()
    {
        if (decoder != nullptr)
        {
            pyrowave_decoder_destroy(decoder);
            decoder = nullptr;
        }
        if (device == VK_NULL_HANDLE)
        {
            return;
        }
        vkQueueWaitIdle(queue);
        for (Image& image : planes)
        {
            releaseImage(image);
        }
        for (Buffer& b : packed)
        {
            releaseBuffer(b);
        }
        releaseBuffer(params);
        releaseBuffer(rgbx);
        releaseImage(frame);
        hasFrame = false;
        width = 0;
        height = 0;
    }

    bool initialize(const GpuContext& ctx)
    {
        context = ctx;
        device = ctx.device;
        queue = ctx.queue;
        volkInitializeCustom(ctx.getInstanceProcAddr);
        volkLoadInstanceOnly(ctx.instance);
        volkLoadDevice(ctx.device);
        vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &memoryProperties);

        pyrowave_device_create_queue_info queues = {ctx.queue, ctx.queueFamily, 0};
        pyrowave_device_create_info info = {};
        info.GetInstanceProcAddr = ctx.getInstanceProcAddr;
        info.instance = ctx.instance;
        info.physical_device = ctx.physicalDevice;
        info.device = ctx.device;
        info.instance_create_info = ctx.instanceInfo;
        info.device_create_info = ctx.deviceInfo;
        info.queue_info = &queues;
        info.queue_info_count = 1;
        if (pyrowave_create_device(&info, &pyro) != PYROWAVE_SUCCESS)
        {
            return false;
        }
        pyrowave_device_set_queue_type(pyro, VK_QUEUE_GRAPHICS_BIT);

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = ctx.queueFamily;
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
        if (vkAllocateCommandBuffers(device, &cmdInfo, &cmd) != VK_SUCCESS)
        {
            return false;
        }

        VkDescriptorSetLayoutBinding bindings[5] = {};
        for (uint32_t i = 0; i < 5; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 5;
        setInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout) != VK_SUCCESS)
        {
            return false;
        }
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(k_yuv420_to_rgbx_spv);
        moduleInfo.pCode = k_yuv420_to_rgbx_spv;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS ||
            vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
        {
            return false;
        }
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout;
        const VkResult made = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        if (made != VK_SUCCESS)
        {
            return false;
        }

        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
        VkDescriptorPoolCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorInfo.maxSets = 1;
        descriptorInfo.poolSizeCount = 2;
        descriptorInfo.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &descriptorInfo, nullptr, &descriptorPool) != VK_SUCCESS)
        {
            return false;
        }
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &setLayout;
        return vkAllocateDescriptorSets(device, &allocInfo, &set) == VK_SUCCESS;
    }

    bool beginCommands()
    {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkResetCommandBuffer(cmd, 0) == VK_SUCCESS && vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS;
    }

    bool submitAndWait()
    {
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        {
            return false;
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkResetFences(device, 1, &fence);
        return vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS &&
               vkWaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000ull) == VK_SUCCESS;
    }

    void barrier(VkCommandBuffer commands, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                 VkAccessFlags dstAccess)
    {
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memory.srcAccessMask = srcAccess;
        memory.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(commands, src, dst, 0, 1, &memory, 0, nullptr, 0, nullptr);
    }

    void transition(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                    VkAccessFlags dstAccess, VkPipelineStageFlags src, VkPipelineStageFlags dst)
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    bool ensureFrame(int w, int h)
    {
        if (decoder != nullptr && w == width && h == height)
        {
            return true;
        }
        releaseFrame();
        pyrowave_decoder_create_info info = {pyro, w, h, PYROWAVE_CHROMA_SUBSAMPLING_420, false};
        if (pyrowave_decoder_create(&info, &decoder) != PYROWAVE_SUCCESS)
        {
            return false;
        }
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (int i = 0; i < 3; ++i)
        {
            const VkDeviceSize bytes = (VkDeviceSize(PlaneWidth(i, w)) * PlaneHeight(i, h) + 3) & ~VkDeviceSize(3);
            if (!makeImage(planes[i], VK_FORMAT_R8_UNORM, PlaneWidth(i, w), PlaneHeight(i, h),
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
                !makeBuffer(packed[i], bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            {
                return false;
            }
        }
        if (!makeBuffer(params, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host) ||
            !makeBuffer(rgbx, VkDeviceSize(w) * h * 4,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
            !makeImage(frame, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(w), uint32_t(h),
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        {
            return false;
        }
        const uint32_t dims[2] = {uint32_t(w), uint32_t(h)};
        std::memcpy(params.mapped, dims, sizeof(dims));

        VkDescriptorBufferInfo infos[5] = {{params.buffer, 0, VK_WHOLE_SIZE},    {packed[0].buffer, 0, VK_WHOLE_SIZE},
                                           {packed[1].buffer, 0, VK_WHOLE_SIZE}, {packed[2].buffer, 0, VK_WHOLE_SIZE},
                                           {rgbx.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[5] = {};
        for (uint32_t i = 0; i < 5; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
        width = w;
        height = h;

        // The planes stay in GENERAL from here on.
        if (!beginCommands())
        {
            return false;
        }
        for (Image& image : planes)
        {
            transition(image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        return submitAndWait();
    }

    bool decode(const uint8_t* data, size_t size)
    {
        pyrowave_decoder_clear(decoder);
        if (pyrowave_decoder_push_packet(decoder, data, size) != PYROWAVE_SUCCESS ||
            !pyrowave_decoder_decode_is_ready(decoder, false) || !beginCommands())
        {
            return false;
        }

        pyrowave_gpu_buffers views = {};
        for (int i = 0; i < 3; ++i)
        {
            views.planes[i].image = planes[i].image;
            views.planes[i].width = PlaneWidth(i, width);
            views.planes[i].height = PlaneHeight(i, height);
            views.planes[i].image_format = VK_FORMAT_R8_UNORM;
            views.planes[i].view_format = VK_FORMAT_R8_UNORM;
            views.planes[i].aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            views.planes[i].swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
            views.planes[i].layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        pyrowave_device_set_command_buffer(pyro, cmd);
        const pyrowave_result decoded = pyrowave_decoder_decode_gpu_buffer(decoder, nullptr, nullptr, &views);
        pyrowave_device_set_command_buffer(pyro, VK_NULL_HANDLE);
        if (decoded != PYROWAVE_SUCCESS)
        {
            vkEndCommandBuffer(cmd);
            return false;
        }

        barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        for (int i = 0; i < 3; ++i)
        {
            VkBufferImageCopy region = {};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {PlaneWidth(i, width), PlaneHeight(i, height), 1};
            vkCmdCopyImageToBuffer(cmd, planes[i].image, VK_IMAGE_LAYOUT_GENERAL, packed[i].buffer, 1, &region);
        }
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, (uint32_t(width) * uint32_t(height) + 63) / 64, 1, 1);
        barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);

        transition(frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy toFrame = {};
        toFrame.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        toFrame.imageExtent = {uint32_t(width), uint32_t(height), 1};
        vkCmdCopyBufferToImage(cmd, rgbx.buffer, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &toFrame);
        transition(frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (!submitAndWait())
        {
            return false;
        }
        hasFrame = true;
        return true;
    }

    bool ensureWhite()
    {
        if (whiteReady)
        {
            return true;
        }
        if (!makeImage(white, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
            !beginCommands())
        {
            return false;
        }
        transition(white.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkClearColorValue colour = {{1.0f, 1.0f, 1.0f, 1.0f}};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
        transition(white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        whiteReady = submitAndWait();
        return whiteReady;
    }

    // Two short white bars crossing at the centre of the view, where the gaze and the pointing hand aim.
    void recordReticle(VkCommandBuffer commands, VkImage target, int32_t cx, int32_t cy, int32_t size)
    {
        if (!ensureWhite())
        {
            return;
        }
        barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        const int32_t half = std::max(6, size / 2);
        const int32_t bars[2][4] = {{cx - half, cy - 1, cx + half, cy + 1}, {cx - 1, cy - half, cx + 1, cy + half}};
        for (const int32_t* bar : bars)
        {
            VkImageBlit blit = {};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {1, 1, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {bar[0], bar[1], 0};
            blit.dstOffsets[1] = {bar[2], bar[3], 1};
            vkCmdBlitImage(commands, white.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        }
    }

    void recordLeftEye(VkCommandBuffer commands, VkImage target, uint32_t targetWidth, uint32_t targetHeight, bool reticle)
    {
        const VkClearColorValue background = {{4.0f / 255.0f, 6.0f / 255.0f, 9.0f / 255.0f, 1.0f}};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1, &range);
        if (!hasFrame)
        {
            return;
        }
        barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        const double eyeWidth = width / 2;
        const double scale = std::min(targetWidth / eyeWidth, targetHeight / double(height));
        const int32_t w = std::max(1, int32_t(eyeWidth * scale));
        const int32_t h = std::max(1, int32_t(height * scale));
        const int32_t x = (int32_t(targetWidth) - w) / 2;
        const int32_t y = (int32_t(targetHeight) - h) / 2;
        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {width / 2, height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        vkCmdBlitImage(commands, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        if (reticle)
        {
            recordReticle(commands, target, x + w / 2, y + h / 2, h / 40);
        }
    }

    bool snapshotLeftEye(std::vector<uint8_t>& rgba, int& outWidth, int& outHeight)
    {
        if (!hasFrame)
        {
            return false;
        }
        Buffer staging;
        const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
        if (!makeBuffer(staging, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !beginCommands())
        {
            releaseBuffer(staging);
            return false;
        }
        const VkBufferCopy region = {0, 0, bytes};
        vkCmdCopyBuffer(cmd, rgbx.buffer, staging.buffer, 1, &region);
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
        const bool ok = submitAndWait();
        if (ok)
        {
            const int eye = width / 2;
            rgba.resize(size_t(eye) * height * 4);
            const uint8_t* source = static_cast<const uint8_t*>(staging.mapped);
            for (int row = 0; row < height; ++row)
            {
                std::memcpy(rgba.data() + size_t(row) * eye * 4, source + size_t(row) * width * 4, size_t(eye) * 4);
            }
            for (size_t i = 3; i < rgba.size(); i += 4)
            {
                rgba[i] = 255;
            }
            outWidth = eye;
            outHeight = height;
        }
        releaseBuffer(staging);
        return ok;
    }
};

GpuDecoder::GpuDecoder() = default;

GpuDecoder::~GpuDecoder() = default;

bool GpuDecoder::initialize(const GpuContext& context, std::string* error)
{
    std::unique_ptr<Gpu> gpu = std::make_unique<Gpu>();
    if (!gpu->initialize(context))
    {
        if (error != nullptr)
        {
            *error = "PyroWave: the shared Vulkan device cannot decode";
        }
        return false;
    }
    gpu_ = std::move(gpu);
    return true;
}

bool GpuDecoder::decode(const uint8_t* data, size_t size)
{
    int w = 0;
    int h = 0;
    return gpu_ && frameSize(data, size, w, h) && (w % 2) == 0 && (h % 2) == 0 && gpu_->ensureFrame(w, h) &&
           gpu_->decode(data, size);
}

bool GpuDecoder::hasFrame() const
{
    return gpu_ && gpu_->hasFrame;
}

int GpuDecoder::width() const
{
    return gpu_ ? gpu_->width : 0;
}

int GpuDecoder::height() const
{
    return gpu_ ? gpu_->height : 0;
}

void GpuDecoder::recordLeftEye(VkCommandBuffer commands, VkImage target, uint32_t targetWidth, uint32_t targetHeight,
                               bool reticle)
{
    if (gpu_)
    {
        gpu_->recordLeftEye(commands, target, targetWidth, targetHeight, reticle);
    }
}

bool GpuDecoder::snapshotLeftEye(std::vector<uint8_t>& rgba, int& width, int& height)
{
    return gpu_ && gpu_->snapshotLeftEye(rgba, width, height);
}

bool GpuDecoder::frameSize(const uint8_t* data, size_t size, int& width, int& height)
{
    if (size < 8)
    {
        return false;
    }
    uint32_t word = 0;
    std::memcpy(&word, data, sizeof(word));
    if ((word >> 31) != 1)
    {
        return false;
    }
    width = static_cast<int>(word & 0x3FFF) + 1;
    height = static_cast<int>((word >> 14) & 0x3FFF) + 1;
    return true;
}

} // namespace xrpilot
