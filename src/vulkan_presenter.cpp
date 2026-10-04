// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "vulkan_presenter.h"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace panelspun::detail {

namespace {

bool fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL onValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++*static_cast<int*>(user);
        std::fprintf(stderr, "vulkan validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

}

bool VulkanPresenter::init(SDL_Window* window, bool validation, bool allFeatures, std::string* error) {
    window_ = window;
    ctx_.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!ctx_.getInstanceProcAddr) return fail(error, std::string("no Vulkan loader: ") + SDL_GetError());

    PFN_vkCreateInstance createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(ctx_.getInstanceProcAddr(nullptr, "vkCreateInstance"));
    PFN_vkEnumerateInstanceLayerProperties enumerateLayers = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        ctx_.getInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties"));
    if (!createInstance || !enumerateLayers) return fail(error, "Vulkan loader lacks vkCreateInstance");

    Uint32 sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts) return fail(error, std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError());
    std::vector<const char*>& exts = instanceExtensions_;
    std::vector<const char*>& layers = layers_;
    exts.assign(sdlExts, sdlExts + sdlExtCount);

    if (validation) {
        std::uint32_t count = 0;
        enumerateLayers(&count, nullptr);
        std::vector<VkLayerProperties> props(count);
        enumerateLayers(&count, props.data());
        bool found = false;
        for (const VkLayerProperties& p : props) found = found || std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        if (!found) return fail(error, "validation requested but VK_LAYER_KHRONOS_validation is not installed");
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo& app = appInfo_;
    app.pApplicationName = "panelspun";
    app.pEngineName = "panelspun";
    app.apiVersion = allFeatures ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
    VkInstanceCreateInfo& ici = instanceInfo_;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    VkResult r = createInstance(&ici, nullptr, &ctx_.instance);
    if (r != VK_SUCCESS) return fail(error, "vkCreateInstance failed: " + std::to_string(r));

#define PANELSPUN_LOAD_INSTANCE(name)                                                                \
    name = reinterpret_cast<PFN_##name>(ctx_.getInstanceProcAddr(ctx_.instance, #name));             \
    if (!name) return fail(error, "missing instance function " #name);
    PANELSPUN_VK_INSTANCE_FUNCS(PANELSPUN_LOAD_INSTANCE)
#undef PANELSPUN_LOAD_INSTANCE
    ctx_.getDeviceProcAddr = vkGetDeviceProcAddr;

    if (validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            ctx_.getInstanceProcAddr(ctx_.instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        mci.pfnUserCallback = onValidation;
        mci.pUserData = &validationErrors_;
        if (!create || create(ctx_.instance, &mci, nullptr, &messenger_) != VK_SUCCESS)
            return fail(error, "could not create the validation messenger");
    }

    if (!SDL_Vulkan_CreateSurface(window_, ctx_.instance, nullptr, &surface_))
        return fail(error, std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());

    std::uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(ctx_.instance, &gpuCount, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(ctx_.instance, &gpuCount, gpus.data());
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (allFeatures && props.apiVersion < VK_API_VERSION_1_3) continue;
        std::uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qCount, qprops.data());
        for (std::uint32_t i = 0; i < qCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface_, &present);
            if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                ctx_.physicalDevice = gpu;
                ctx_.queueFamily = i;
                break;
            }
        }
        if (ctx_.physicalDevice) break;
    }
    if (!ctx_.physicalDevice) return fail(error, "no Vulkan device can present to this window");
    vkGetPhysicalDeviceMemoryProperties(ctx_.physicalDevice, &memProps_);

    VkDeviceQueueCreateInfo& qci = queueInfo_;
    qci.queueFamilyIndex = ctx_.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &queuePriority_;
    VkDeviceCreateInfo& dci = deviceInfo_;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = deviceExtensions_;
    if (allFeatures) {
        // Every supported core feature, as compute libraries sharing the device expect.
        features_.pNext = &features11_;
        features11_.pNext = &features12_;
        features12_.pNext = &features13_;
        vkGetPhysicalDeviceFeatures2(ctx_.physicalDevice, &features_);
        features_.features.robustBufferAccess = VK_FALSE;
        dci.pNext = &features_;
    }
    r = vkCreateDevice(ctx_.physicalDevice, &dci, nullptr, &ctx_.device);
    if (r != VK_SUCCESS) return fail(error, "vkCreateDevice failed: " + std::to_string(r));

#define PANELSPUN_LOAD_DEVICE(name)                                                    \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(ctx_.device, #name));      \
    if (!name) return fail(error, "missing device function " #name);
    PANELSPUN_VK_DEVICE_FUNCS(PANELSPUN_LOAD_DEVICE)
#undef PANELSPUN_LOAD_DEVICE

    vkGetDeviceQueue(ctx_.device, ctx_.queueFamily, 0, &ctx_.queue);
    ctx_.apiVersion = app.apiVersion;
    ctx_.instanceInfo = &instanceInfo_;
    ctx_.deviceInfo = &deviceInfo_;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx_.queueFamily;
    if (vkCreateCommandPool(ctx_.device, &pci, nullptr, &pool_) != VK_SUCCESS) return fail(error, "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx_.device, &cai, &cmd_) != VK_SUCCESS) return fail(error, "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(ctx_.device, &fci, nullptr, &frameFence_) != VK_SUCCESS) return fail(error, "vkCreateFence");
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(ctx_.device, &sci, nullptr, &acquired_) != VK_SUCCESS) return fail(error, "vkCreateSemaphore");
    return true;
}

std::uint32_t VulkanPresenter::memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (std::uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

bool VulkanPresenter::createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, HostBuffer& out) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ctx_.device, &bci, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(ctx_.device, out.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(ctx_.device, &mai, nullptr, &out.memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(ctx_.device, out.buffer, out.memory, 0) != VK_SUCCESS) return false;
    if (vkMapMemory(ctx_.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) return false;
    out.size = size;
    return true;
}

void VulkanPresenter::destroyHostBuffer(HostBuffer& b) {
    if (b.mapped) vkUnmapMemory(ctx_.device, b.memory);
    if (b.buffer) vkDestroyBuffer(ctx_.device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(ctx_.device, b.memory, nullptr);
    b = HostBuffer{};
}

bool VulkanPresenter::createRegionImage(std::uint32_t w, std::uint32_t h, RegionImage& out) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format_;
    ici.extent = VkExtent3D{w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx_.device, &ici, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx_.device, out.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = memoryType(req.memoryTypeBits, 0);
    if (vkAllocateMemory(ctx_.device, &mai, nullptr, &out.memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(ctx_.device, out.image, out.memory, 0) != VK_SUCCESS) return false;
    out.width = w;
    out.height = h;
    return true;
}

void VulkanPresenter::destroyRegionImage(RegionImage& r) {
    if (r.image) vkDestroyImage(ctx_.device, r.image, nullptr);
    if (r.memory) vkFreeMemory(ctx_.device, r.memory, nullptr);
    r = RegionImage{};
}

void VulkanPresenter::destroySwapchainResources() {
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(ctx_.device, s, nullptr);
    renderDone_.clear();
    images_.clear();
    destroyHostBuffer(ui_);
    destroyHostBuffer(readback_);
}

bool VulkanPresenter::ensureSwapchain(int width, int height, std::string* error) {
    if (width <= 0 || height <= 0) return false;
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physicalDevice, surface_, &caps);
    VkExtent2D want{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    if (caps.currentExtent.width != UINT32_MAX) want = caps.currentExtent;
    want.width = std::clamp(want.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    want.height = std::clamp(want.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (want.width == 0 || want.height == 0) return false;
    if (!stale_ && swapchain_ && want.width == extent_.width && want.height == extent_.height) return true;

    vkDeviceWaitIdle(ctx_.device);
    destroySwapchainResources();

    if (format_ == VK_FORMAT_UNDEFINED) {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physicalDevice, surface_, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physicalDevice, surface_, &count, formats.data());
        for (VkFormat pref : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            for (const VkSurfaceFormatKHR& f : formats)
                if (f.format == pref && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && format_ == VK_FORMAT_UNDEFINED)
                    format_ = pref;
        }
        if (format_ == VK_FORMAT_UNDEFINED) return fail(error, "surface offers neither BGRA8 nor RGBA8 UNORM");
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return fail(error, "swapchain images cannot be transfer destinations on this surface");
    canCapture_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = format_;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = want;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (canCapture_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (std::uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1) {
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) break;
        if (caps.supportedCompositeAlpha & bit) {
            sci.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
            break;
        }
    }
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR next = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(ctx_.device, &sci, nullptr, &next);
    if (swapchain_) vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
    swapchain_ = next;
    if (r != VK_SUCCESS) return fail(error, "vkCreateSwapchainKHR failed: " + std::to_string(r));
    extent_ = want;

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &count, images_.data());
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderDone_.resize(count, VK_NULL_HANDLE);
    for (VkSemaphore& s : renderDone_)
        if (vkCreateSemaphore(ctx_.device, &semInfo, nullptr, &s) != VK_SUCCESS) return fail(error, "vkCreateSemaphore");

    VkDeviceSize bytes = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
    if (!createHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, ui_)) return fail(error, "could not allocate the UI buffer");
    if (canCapture_ && !createHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback_))
        return fail(error, "could not allocate the readback buffer");
    stale_ = false;
    return true;
}

void VulkanPresenter::waitFrame() {
    if (ctx_.device) vkWaitForFences(ctx_.device, 1, &frameFence_, VK_TRUE, UINT64_MAX);
}

void VulkanPresenter::barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                              VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool VulkanPresenter::present(const std::vector<RegionRequest>& regions, const std::string& capturePath, bool* captured) {
    if (captured) *captured = false;
    std::uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(ctx_.device, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        stale_ = true;
        return true;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;

    // Images of panels that left the layout or changed size are released; the previous frame is complete.
    for (std::map<Panel*, RegionImage>::iterator it = regions_.begin(); it != regions_.end();) {
        bool keep = false;
        for (const RegionRequest& q : regions)
            keep = keep || (q.panel == it->first && static_cast<std::uint32_t>(q.rect.w) == it->second.width &&
                            static_cast<std::uint32_t>(q.rect.h) == it->second.height);
        if (keep) {
            ++it;
        } else {
            destroyRegionImage(it->second);
            it = regions_.erase(it);
        }
    }

    vkResetFences(ctx_.device, 1, &frameFence_);
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &begin);

    VkImage target = images_[index];
    barrier(cmd_, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy full{};
    full.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    full.imageExtent = VkExtent3D{extent_.width, extent_.height, 1};
    vkCmdCopyBufferToImage(cmd_, ui_.buffer, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full);

    for (const RegionRequest& q : regions) {
        Rect rc = q.rect;
        rc.w = std::min(rc.w, static_cast<int>(extent_.width) - rc.x);
        rc.h = std::min(rc.h, static_cast<int>(extent_.height) - rc.y);
        if (rc.x < 0 || rc.y < 0 || rc.w <= 0 || rc.h <= 0) continue;
        RegionImage& img = regions_[q.panel];
        if (!img.image && !createRegionImage(static_cast<std::uint32_t>(q.rect.w), static_cast<std::uint32_t>(q.rect.h), img)) {
            destroyRegionImage(img);
            regions_.erase(q.panel);
            continue;
        }
        barrier(cmd_, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VulkanRegionFrame frame;
        frame.context = &ctx_;
        frame.commands = cmd_;
        frame.image = img.image;
        frame.format = format_;
        frame.width = img.width;
        frame.height = img.height;
        frame.frameIndex = frameIndex_;
        q.panel->recordVulkan(frame);
        barrier(cmd_, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.dstOffset = VkOffset3D{rc.x, rc.y, 0};
        copy.extent = VkExtent3D{static_cast<std::uint32_t>(rc.w), static_cast<std::uint32_t>(rc.h), 1};
        vkCmdCopyImage(cmd_, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &copy);
    }

    bool capture = !capturePath.empty() && canCapture_;
    if (capture) {
        barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &full);
        barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
        barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
                0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    vkEndCommandBuffer(cmd_);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderDone_[index];
    if (vkQueueSubmit(ctx_.queue, 1, &submit, frameFence_) != VK_SUCCESS) return false;
    ++frameIndex_;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(ctx_.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) stale_ = true;
    else if (r != VK_SUCCESS) return false;

    if (capture) {
        waitFrame();
        SDL_Surface* s = SDL_CreateSurfaceFrom(static_cast<int>(extent_.width), static_cast<int>(extent_.height),
                                               bgra() ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888, readback_.mapped,
                                               static_cast<int>(extent_.width * 4));
        if (s) {
            SDL_Surface* opaque = SDL_ConvertSurface(s, SDL_PIXELFORMAT_XRGB8888);
            if (captured && opaque) *captured = SDL_SaveBMP(opaque, capturePath.c_str());
            SDL_DestroySurface(opaque);
            SDL_DestroySurface(s);
        }
    }
    return true;
}

void VulkanPresenter::shutdown() {
    if (ctx_.device) {
        vkDeviceWaitIdle(ctx_.device);
        for (std::pair<Panel* const, RegionImage>& kv : regions_) destroyRegionImage(kv.second);
        regions_.clear();
        destroySwapchainResources();
        if (swapchain_) vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
        if (acquired_) vkDestroySemaphore(ctx_.device, acquired_, nullptr);
        if (frameFence_) vkDestroyFence(ctx_.device, frameFence_, nullptr);
        if (pool_) vkDestroyCommandPool(ctx_.device, pool_, nullptr);
        vkDestroyDevice(ctx_.device, nullptr);
    }
    if (ctx_.instance) {
        if (surface_) SDL_Vulkan_DestroySurface(ctx_.instance, surface_, nullptr);
        if (messenger_) {
            PFN_vkDestroyDebugUtilsMessengerEXT destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                ctx_.getInstanceProcAddr(ctx_.instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(ctx_.instance, messenger_, nullptr);
        }
        if (vkDestroyInstance) vkDestroyInstance(ctx_.instance, nullptr);
    }
    ctx_ = VulkanContext{};
    swapchain_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
}

}
