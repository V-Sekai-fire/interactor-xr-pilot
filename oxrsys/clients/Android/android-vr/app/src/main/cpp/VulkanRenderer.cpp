// SPDX-License-Identifier: MPL-2.0

#include "VulkanRenderer.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>

#define LOG_TAG "OXRSys-Vulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace oxr
{

namespace
{

// From shaders/gen.sh.
const uint32_t kBlitVert[] = {
#include "shaders/spirv/blit_vert.inc"
};
const uint32_t kBlitFrag[] = {
#include "shaders/spirv/blit_frag.inc"
};
const uint32_t kShellVert[] = {
#include "shaders/spirv/shell_vert.inc"
};
const uint32_t kShellFrag[] = {
#include "shaders/spirv/shell_frag.inc"
};

constexpr uint64_t kFenceTimeoutNs = 1'000'000'000ull;

template <typename Fn> bool XrProc(XrInstance instance, const char* name, Fn* fn)
{
    return XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(fn))) &&
           *fn != nullptr;
}

} // namespace

VulkanRenderer::~VulkanRenderer()
{
    Shutdown();
}

bool VulkanRenderer::CreateDevice(XrInstance instance, XrSystemId systemId)
{
    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
    if (!XrProc(instance, "xrGetVulkanGraphicsRequirements2KHR", &getRequirements) ||
        !XrProc(instance, "xrCreateVulkanInstanceKHR", &createInstance) ||
        !XrProc(instance, "xrGetVulkanGraphicsDevice2KHR", &getDevice) ||
        !XrProc(instance, "xrCreateVulkanDeviceKHR", &createDevice))
    {
        LOGE("XR_KHR_vulkan_enable2 functions missing");
        return false;
    }

    // OpenXR requires the requirements be asked before the session.
    XrGraphicsRequirementsVulkan2KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (XR_FAILED(getRequirements(instance, systemId, &requirements)))
    {
        LOGE("xrGetVulkanGraphicsRequirements2KHR failed");
        return false;
    }
    LOGI("Vulkan requirements: min %u.%u, max %u.%u", XR_VERSION_MAJOR(requirements.minApiVersionSupported),
         XR_VERSION_MINOR(requirements.minApiVersionSupported), XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
         XR_VERSION_MINOR(requirements.maxApiVersionSupported));

    if (volkInitialize() != VK_SUCCESS)
    {
        LOGE("No Vulkan loader");
        return false;
    }

    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "OXRSys Android";
    appInfo.pEngineName = "OXRSys";
    appInfo.apiVersion = VK_API_VERSION_1_1; // negative viewport heights
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    XrVulkanInstanceCreateInfoKHR xrInstanceInfo = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceInfo.systemId = systemId;
    xrInstanceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
    VkResult vkResult = VK_SUCCESS;
    if (XR_FAILED(createInstance(instance, &xrInstanceInfo, &instance_, &vkResult)) || vkResult != VK_SUCCESS)
    {
        LOGE("xrCreateVulkanInstanceKHR failed (VkResult %d)", vkResult);
        return false;
    }
    // Only this thread's setup reads volk's globals; the device table below keeps its own pointers.
    volkLoadInstanceOnly(instance_);
    destroyInstance_ = vkDestroyInstance;

    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = systemId;
    deviceGetInfo.vulkanInstance = instance_;
    if (XR_FAILED(getDevice(instance, &deviceGetInfo, &physical_)))
    {
        LOGE("xrGetVulkanGraphicsDevice2KHR failed");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties_);

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, families.data());
    uint32_t family = familyCount;
    for (uint32_t i = 0; i < familyCount; ++i)
    {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            family = i;
            break;
        }
    }
    if (family == familyCount)
    {
        LOGE("No graphics queue");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    XrVulkanDeviceCreateInfoKHR xrDeviceInfo = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceInfo.systemId = systemId;
    xrDeviceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrDeviceInfo.vulkanPhysicalDevice = physical_;
    xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
    if (XR_FAILED(createDevice(instance, &xrDeviceInfo, &device_, &vkResult)) || vkResult != VK_SUCCESS)
    {
        LOGE("xrCreateVulkanDeviceKHR failed (VkResult %d)", vkResult);
        return false;
    }
    volkLoadDeviceTable(&vk_, device_);
    vk_.vkGetDeviceQueue(device_, family, 0, &queue_);

    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    if (vk_.vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
    {
        return false;
    }
    VkCommandBuffer buffers[3] = {};
    VkCommandBufferAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = pool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 3;
    if (vk_.vkAllocateCommandBuffers(device_, &allocateInfo, buffers) != VK_SUCCESS)
    {
        return false;
    }
    eyes_[0].cmd = buffers[0];
    eyes_[1].cmd = buffers[1];
    uploadCmd_ = buffers[2];
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (Eye& eye : eyes_)
    {
        if (vk_.vkCreateFence(device_, &fenceInfo, nullptr, &eye.fence) != VK_SUCCESS)
        {
            return false;
        }
    }
    if (vk_.vkCreateFence(device_, &fenceInfo, nullptr, &uploadFence_) != VK_SUCCESS)
    {
        return false;
    }

    binding_.instance = instance_;
    binding_.physicalDevice = physical_;
    binding_.device = device_;
    binding_.queueFamilyIndex = family;
    binding_.queueIndex = 0;
    LOGI("Vulkan device created for OpenXR (queue family %u)", family);
    return true;
}

int64_t VulkanRenderer::ChooseSwapchainFormat(XrSession session) const
{
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(session, count, &count, formats.data());
    for (int64_t wanted : {int64_t(VK_FORMAT_R8G8B8A8_SRGB), int64_t(VK_FORMAT_B8G8R8A8_SRGB)})
    {
        if (std::find(formats.begin(), formats.end(), wanted) != formats.end())
        {
            return wanted;
        }
    }
    return formats.empty() ? int64_t(VK_FORMAT_R8G8B8A8_SRGB) : formats[0];
}

bool VulkanRenderer::AdoptSwapchain(int eye, XrSwapchain swapchain, int64_t format, uint32_t width, uint32_t height)
{
    if (renderPass_ == VK_NULL_HANDLE && !CreatePipelines(static_cast<VkFormat>(format)))
    {
        LOGE("Creating the render pass and pipelines failed");
        return false;
    }

    uint32_t count = 0;
    xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    // A session that becomes ready again brings new swapchains.
    Eye& target = eyes_[eye];
    ReleaseEye(target);
    target.width = width;
    target.height = height;
    for (const XrSwapchainImageVulkan2KHR& image : images)
    {
        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vk_.vkCreateImageView(device_, &viewInfo, nullptr, &view) != VK_SUCCESS)
        {
            return false;
        }
        VkFramebufferCreateInfo framebufferInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &view;
        framebufferInfo.width = width;
        framebufferInfo.height = height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (vk_.vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS)
        {
            vk_.vkDestroyImageView(device_, view, nullptr);
            return false;
        }
        target.images.push_back(image.image);
        target.views.push_back(view);
        target.framebuffers.push_back(framebuffer);
    }
    return true;
}

bool VulkanRenderer::CreatePipelines(VkFormat format)
{
    format_ = format;

    // Cleared on load; OpenXR takes a released colour image in COLOR_ATTACHMENT_OPTIMAL.
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colourRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colourRef;
    VkRenderPassCreateInfo passInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    if (vk_.vkCreateRenderPass(device_, &passInfo, nullptr, &renderPass_) != VK_SUCCESS)
    {
        return false;
    }

    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vk_.vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS)
    {
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[VideoDecoder::PlaneCount] = {};
    for (uint32_t i = 0; i < VideoDecoder::PlaneCount; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setLayoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutInfo.bindingCount = VideoDecoder::PlaneCount;
    setLayoutInfo.pBindings = bindings;
    if (vk_.vkCreateDescriptorSetLayout(device_, &setLayoutInfo, nullptr, &planeLayout_) != VK_SUCCESS)
    {
        return false;
    }
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VideoDecoder::PlaneCount};
    VkDescriptorPoolCreateInfo descriptorPoolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptorPoolInfo.maxSets = 1;
    descriptorPoolInfo.poolSizeCount = 1;
    descriptorPoolInfo.pPoolSizes = &poolSize;
    if (vk_.vkCreateDescriptorPool(device_, &descriptorPoolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
    {
        return false;
    }
    VkDescriptorSetAllocateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &planeLayout_;
    if (vk_.vkAllocateDescriptorSets(device_, &setInfo, &planeSet_) != VK_SUCCESS)
    {
        return false;
    }

    VkPushConstantRange blitRange = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlitParams)};
    VkPipelineLayoutCreateInfo blitLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    blitLayoutInfo.setLayoutCount = 1;
    blitLayoutInfo.pSetLayouts = &planeLayout_;
    blitLayoutInfo.pushConstantRangeCount = 1;
    blitLayoutInfo.pPushConstantRanges = &blitRange;
    VkPushConstantRange shellRange = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(float)};
    VkPipelineLayoutCreateInfo shellLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    shellLayoutInfo.pushConstantRangeCount = 1;
    shellLayoutInfo.pPushConstantRanges = &shellRange;
    if (vk_.vkCreatePipelineLayout(device_, &blitLayoutInfo, nullptr, &blitLayout_) != VK_SUCCESS ||
        vk_.vkCreatePipelineLayout(device_, &shellLayoutInfo, nullptr, &shellLayout_) != VK_SUCCESS)
    {
        return false;
    }

    auto module = [this](const uint32_t* code, size_t size)
    {
        VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = size;
        info.pCode = code;
        VkShaderModule shader = VK_NULL_HANDLE;
        vk_.vkCreateShaderModule(device_, &info, nullptr, &shader);
        return shader;
    };
    VkShaderModule blitVert = module(kBlitVert, sizeof(kBlitVert));
    VkShaderModule blitFrag = module(kBlitFrag, sizeof(kBlitFrag));
    VkShaderModule shellVert = module(kShellVert, sizeof(kShellVert));
    VkShaderModule shellFrag = module(kShellFrag, sizeof(kShellFrag));
    if (blitVert != VK_NULL_HANDLE && blitFrag != VK_NULL_HANDLE && shellVert != VK_NULL_HANDLE &&
        shellFrag != VK_NULL_HANDLE)
    {
        blitPipeline_ =
            CreatePipeline(blitVert, blitFrag, blitLayout_, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false);
        shellTrianglePipeline_ =
            CreatePipeline(shellVert, shellFrag, shellLayout_, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, true);
        shellLinePipeline_ =
            CreatePipeline(shellVert, shellFrag, shellLayout_, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, true, true);
    }
    for (VkShaderModule shader : {blitVert, blitFrag, shellVert, shellFrag})
    {
        vk_.vkDestroyShaderModule(device_, shader, nullptr);
    }
    if (blitPipeline_ == VK_NULL_HANDLE || shellTrianglePipeline_ == VK_NULL_HANDLE ||
        shellLinePipeline_ == VK_NULL_HANDLE)
    {
        return false;
    }
    LOGI("Vulkan pipelines created (swapchain format %d)", int(format));
    return true;
}

VkPipeline VulkanRenderer::CreatePipeline(VkShaderModule vertex, VkShaderModule fragment, VkPipelineLayout layout,
                                          VkPrimitiveTopology topology, bool shellVertices, bool blend)
{
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertexBinding = {0, sizeof(ShellVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 3 * sizeof(float)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (shellVertices)
    {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &vertexBinding;
        vertexInput.vertexAttributeDescriptionCount = 2;
        vertexInput.pVertexAttributeDescriptions = attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // GL's glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), which blends alpha the same way.
    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.blendEnable = blend ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blendState = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blendState;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = renderPass_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vk_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

int32_t VulkanRenderer::MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i)
    {
        if ((bits & (1u << i)) != 0 && (memoryProperties_.memoryTypes[i].propertyFlags & flags) == flags)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool VulkanRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer)
{
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    if (vk_.vkCreateBuffer(device_, &info, nullptr, &buffer->buffer) != VK_SUCCESS)
    {
        return false;
    }
    VkMemoryRequirements requirements = {};
    vk_.vkGetBufferMemoryRequirements(device_, buffer->buffer, &requirements);
    const int32_t type = MemoryType(requirements.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = static_cast<uint32_t>(type);
    if (type < 0 || vk_.vkAllocateMemory(device_, &allocateInfo, nullptr, &buffer->memory) != VK_SUCCESS ||
        vk_.vkBindBufferMemory(device_, buffer->buffer, buffer->memory, 0) != VK_SUCCESS ||
        vk_.vkMapMemory(device_, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped) != VK_SUCCESS)
    {
        DestroyBuffer(buffer);
        return false;
    }
    buffer->size = size;
    return true;
}

void VulkanRenderer::DestroyBuffer(Buffer* buffer)
{
    if (buffer->buffer != VK_NULL_HANDLE)
    {
        vk_.vkDestroyBuffer(device_, buffer->buffer, nullptr);
    }
    if (buffer->memory != VK_NULL_HANDLE)
    {
        vk_.vkFreeMemory(device_, buffer->memory, nullptr); // unmaps
    }
    *buffer = {};
}

bool VulkanRenderer::CreatePlane(uint32_t width, uint32_t height, Plane* plane)
{
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk_.vkCreateImage(device_, &info, nullptr, &plane->image) != VK_SUCCESS)
    {
        return false;
    }
    VkMemoryRequirements requirements = {};
    vk_.vkGetImageMemoryRequirements(device_, plane->image, &requirements);
    const int32_t type = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = static_cast<uint32_t>(type);
    if (type < 0 || vk_.vkAllocateMemory(device_, &allocateInfo, nullptr, &plane->memory) != VK_SUCCESS ||
        vk_.vkBindImageMemory(device_, plane->image, plane->memory, 0) != VK_SUCCESS)
    {
        DestroyPlane(plane);
        return false;
    }
    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = plane->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk_.vkCreateImageView(device_, &viewInfo, nullptr, &plane->view) != VK_SUCCESS)
    {
        DestroyPlane(plane);
        return false;
    }
    plane->width = width;
    plane->height = height;
    return true;
}

void VulkanRenderer::DestroyPlane(Plane* plane)
{
    if (plane->view != VK_NULL_HANDLE)
    {
        vk_.vkDestroyImageView(device_, plane->view, nullptr);
    }
    if (plane->image != VK_NULL_HANDLE)
    {
        vk_.vkDestroyImage(device_, plane->image, nullptr);
    }
    if (plane->memory != VK_NULL_HANDLE)
    {
        vk_.vkFreeMemory(device_, plane->memory, nullptr);
    }
    *plane = {};
}

void VulkanRenderer::WaitEye(Eye& eye)
{
    if (eye.submitted)
    {
        vk_.vkWaitForFences(device_, 1, &eye.fence, VK_TRUE, kFenceTimeoutNs);
        vk_.vkResetFences(device_, 1, &eye.fence);
        eye.submitted = false;
    }
}

bool VulkanRenderer::UploadPlanes(const VideoDecoder::DecodedFrame& frame)
{
    if (device_ == VK_NULL_HANDLE || planeSet_ == VK_NULL_HANDLE)
    {
        return false;
    }
    // The last frame's eyes sample the planes, and its upload reads the staging buffer.
    WaitEye(eyes_[0]);
    WaitEye(eyes_[1]);
    if (uploadSubmitted_)
    {
        vk_.vkWaitForFences(device_, 1, &uploadFence_, VK_TRUE, kFenceTimeoutNs);
        vk_.vkResetFences(device_, 1, &uploadFence_);
        uploadSubmitted_ = false;
    }

    VkDeviceSize total = 0;
    VkDeviceSize offsets[VideoDecoder::PlaneCount] = {};
    bool resized = false;
    for (uint32_t i = 0; i < VideoDecoder::PlaneCount; ++i)
    {
        if (frame.planes[i] == nullptr || frame.planeWidth[i] == 0 || frame.planeHeight[i] == 0)
        {
            return false;
        }
        offsets[i] = total;
        total += VkDeviceSize(frame.planeWidth[i]) * frame.planeHeight[i];
        total = (total + 3) & ~VkDeviceSize(3); // copy offsets are multiples of 4
        resized = resized || planes_[i].width != frame.planeWidth[i] || planes_[i].height != frame.planeHeight[i];
    }
    if (staging_.size < total)
    {
        DestroyBuffer(&staging_);
        if (!CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging_))
        {
            LOGE("Creating a %llu-byte upload buffer failed", static_cast<unsigned long long>(total));
            return false;
        }
    }
    // A new size comes with a stream config change; the waits above left nothing sampling the old planes.
    if (resized)
    {
        VkDescriptorImageInfo imageInfos[VideoDecoder::PlaneCount] = {};
        VkWriteDescriptorSet writes[VideoDecoder::PlaneCount] = {};
        for (uint32_t i = 0; i < VideoDecoder::PlaneCount; ++i)
        {
            DestroyPlane(&planes_[i]);
            if (!CreatePlane(frame.planeWidth[i], frame.planeHeight[i], &planes_[i]))
            {
                LOGE("Creating plane %u (%ux%u) failed", i, frame.planeWidth[i], frame.planeHeight[i]);
                return false;
            }
            imageInfos[i] = {sampler_, planes_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = planeSet_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &imageInfos[i];
        }
        vk_.vkUpdateDescriptorSets(device_, VideoDecoder::PlaneCount, writes, 0, nullptr);
        LOGI("Plane textures %ux%u, %ux%u, %ux%u", planes_[0].width, planes_[0].height, planes_[1].width,
             planes_[1].height, planes_[2].width, planes_[2].height);
    }

    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.vkBeginCommandBuffer(uploadCmd_, &begin);
    VkImageMemoryBarrier barriers[VideoDecoder::PlaneCount] = {};
    for (uint32_t i = 0; i < VideoDecoder::PlaneCount; ++i)
    {
        std::memcpy(static_cast<uint8_t*>(staging_.mapped) + offsets[i], frame.planes[i],
                    size_t(frame.planeWidth[i]) * frame.planeHeight[i]);
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcAccessMask = 0;
        barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = planes_[i].image;
        barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vk_.vkCmdPipelineBarrier(uploadCmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, VideoDecoder::PlaneCount, barriers);
    for (uint32_t i = 0; i < VideoDecoder::PlaneCount; ++i)
    {
        VkBufferImageCopy region = {};
        region.bufferOffset = offsets[i];
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {frame.planeWidth[i], frame.planeHeight[i], 1};
        vk_.vkCmdCopyBufferToImage(uploadCmd_, staging_.buffer, planes_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &region);
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    // Later submissions on the queue (both eyes) see the planes through this barrier.
    vk_.vkCmdPipelineBarrier(uploadCmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, VideoDecoder::PlaneCount, barriers);
    vk_.vkEndCommandBuffer(uploadCmd_);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &uploadCmd_;
    if (vk_.vkQueueSubmit(queue_, 1, &submit, uploadFence_) != VK_SUCCESS)
    {
        LOGE("Submitting the plane upload failed");
        return false;
    }
    uploadSubmitted_ = true;
    return true;
}

void VulkanRenderer::BeginEye(int eye, uint32_t imageIndex, const float clearColour[4])
{
    Eye& target = eyes_[eye];
    WaitEye(target);
    currentEye_ = eye;

    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.vkBeginCommandBuffer(target.cmd, &begin);
    VkClearValue clear = {};
    std::memcpy(clear.color.float32, clearColour, sizeof(clear.color.float32));
    VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = renderPass_;
    pass.framebuffer = target.framebuffers[imageIndex];
    pass.renderArea = {{0, 0}, {target.width, target.height}};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vk_.vkCmdBeginRenderPass(target.cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    SetViewport(0, 0, target.width, target.height);
}

void VulkanRenderer::SetViewport(int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    const Eye& target = eyes_[currentEye_];
    // A negative height flips y, so clip-space y points up as in GL; the rectangle's top row is image row
    // (image height - y - height).
    const int32_t top = static_cast<int32_t>(target.height) - y - static_cast<int32_t>(height);
    VkViewport viewport = {};
    viewport.x = static_cast<float>(x);
    viewport.y = static_cast<float>(top + static_cast<int32_t>(height));
    viewport.width = static_cast<float>(width);
    viewport.height = -static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vk_.vkCmdSetViewport(target.cmd, 0, 1, &viewport);
    VkRect2D scissor = {};
    scissor.offset = {std::max(x, 0), std::max(top, 0)};
    scissor.extent = {std::min(width, target.width - uint32_t(scissor.offset.x)),
                      std::min(height, target.height - uint32_t(scissor.offset.y))};
    vk_.vkCmdSetScissor(target.cmd, 0, 1, &scissor);
}

void VulkanRenderer::DrawVideo(const BlitParams& params)
{
    // Nothing to sample before the first upload.
    if (planes_[0].image == VK_NULL_HANDLE)
    {
        return;
    }
    const VkCommandBuffer cmd = eyes_[currentEye_].cmd;
    vk_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipeline_);
    vk_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, 0, 1, &planeSet_, 0, nullptr);
    vk_.vkCmdPushConstants(cmd, blitLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlitParams), &params);
    vk_.vkCmdDraw(cmd, 3, 1, 0, 0);
}

void VulkanRenderer::DrawShell(const float mvp[16], const std::vector<ShellVertex>& triangles,
                               const std::vector<ShellVertex>& lines)
{
    Eye& target = eyes_[currentEye_];
    const VkDeviceSize needed = (triangles.size() + lines.size()) * sizeof(ShellVertex);
    if (needed == 0)
    {
        return;
    }
    // BeginEye waited for this eye's last frame, so its vertex buffer is free to rewrite.
    if (target.shellVertices.size < needed)
    {
        DestroyBuffer(&target.shellVertices);
        if (!CreateBuffer(needed * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &target.shellVertices))
        {
            LOGE("Creating the shell's vertex buffer failed");
            return;
        }
    }
    auto* mapped = static_cast<ShellVertex*>(target.shellVertices.mapped);
    std::copy(triangles.begin(), triangles.end(), mapped);
    std::copy(lines.begin(), lines.end(), mapped + triangles.size());

    const VkCommandBuffer cmd = target.cmd;
    const VkDeviceSize offset = 0;
    vk_.vkCmdBindVertexBuffers(cmd, 0, 1, &target.shellVertices.buffer, &offset);
    if (!triangles.empty())
    {
        vk_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shellTrianglePipeline_);
        vk_.vkCmdPushConstants(cmd, shellLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(float), mvp);
        vk_.vkCmdDraw(cmd, static_cast<uint32_t>(triangles.size()), 1, 0, 0);
    }
    if (!lines.empty())
    {
        vk_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shellLinePipeline_);
        vk_.vkCmdPushConstants(cmd, shellLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(float), mvp);
        vk_.vkCmdDraw(cmd, static_cast<uint32_t>(lines.size()), 1, static_cast<uint32_t>(triangles.size()), 0);
    }
}

void VulkanRenderer::EndEye()
{
    Eye& target = eyes_[currentEye_];
    vk_.vkCmdEndRenderPass(target.cmd);
    vk_.vkEndCommandBuffer(target.cmd);
    // The runtime orders its reads of the released image after this queue's work.
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &target.cmd;
    if (vk_.vkQueueSubmit(queue_, 1, &submit, target.fence) == VK_SUCCESS)
    {
        target.submitted = true;
    }
    else
    {
        LOGE("Submitting eye %d failed", currentEye_);
    }
    currentEye_ = -1;
}

void VulkanRenderer::ReleaseEye(Eye& eye)
{
    WaitEye(eye);
    for (VkFramebuffer framebuffer : eye.framebuffers)
    {
        vk_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (VkImageView view : eye.views)
    {
        vk_.vkDestroyImageView(device_, view, nullptr);
    }
    eye.images.clear();
    eye.views.clear();
    eye.framebuffers.clear();
}

void VulkanRenderer::ReleaseSwapchains()
{
    if (device_ == VK_NULL_HANDLE)
    {
        return;
    }
    vk_.vkDeviceWaitIdle(device_);
    for (Eye& eye : eyes_)
    {
        ReleaseEye(eye);
    }
}

void VulkanRenderer::Shutdown()
{
    if (device_ != VK_NULL_HANDLE)
    {
        ReleaseSwapchains();
        for (Eye& eye : eyes_)
        {
            DestroyBuffer(&eye.shellVertices);
            if (eye.fence != VK_NULL_HANDLE)
            {
                vk_.vkDestroyFence(device_, eye.fence, nullptr);
            }
            eye = {};
        }
        for (Plane& plane : planes_)
        {
            DestroyPlane(&plane);
        }
        DestroyBuffer(&staging_);
        if (uploadFence_ != VK_NULL_HANDLE)
        {
            vk_.vkDestroyFence(device_, uploadFence_, nullptr);
        }
        for (VkPipeline pipeline : {blitPipeline_, shellTrianglePipeline_, shellLinePipeline_})
        {
            vk_.vkDestroyPipeline(device_, pipeline, nullptr);
        }
        vk_.vkDestroyPipelineLayout(device_, blitLayout_, nullptr);
        vk_.vkDestroyPipelineLayout(device_, shellLayout_, nullptr);
        vk_.vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        vk_.vkDestroyDescriptorSetLayout(device_, planeLayout_, nullptr);
        vk_.vkDestroySampler(device_, sampler_, nullptr);
        vk_.vkDestroyRenderPass(device_, renderPass_, nullptr);
        vk_.vkDestroyCommandPool(device_, pool_, nullptr);
        vk_.vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE && destroyInstance_ != nullptr)
    {
        destroyInstance_(instance_, nullptr);
    }
    binding_ = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    instance_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    vk_ = {};
    destroyInstance_ = nullptr;
    pool_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;
    format_ = VK_FORMAT_UNDEFINED;
    sampler_ = VK_NULL_HANDLE;
    planeLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    planeSet_ = VK_NULL_HANDLE;
    blitLayout_ = VK_NULL_HANDLE;
    shellLayout_ = VK_NULL_HANDLE;
    blitPipeline_ = VK_NULL_HANDLE;
    shellTrianglePipeline_ = VK_NULL_HANDLE;
    shellLinePipeline_ = VK_NULL_HANDLE;
    currentEye_ = -1;
    uploadCmd_ = VK_NULL_HANDLE;
    uploadFence_ = VK_NULL_HANDLE;
    uploadSubmitted_ = false;
}

} // namespace oxr
