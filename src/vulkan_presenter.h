// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "panelspun/panel.h"
#include "panelspun/vulkan_region.h"

namespace panelspun::detail {

#define PANELSPUN_VK_INSTANCE_FUNCS(X)          \
    X(vkDestroyInstance)                        \
    X(vkEnumeratePhysicalDevices)               \
    X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties)      \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)     \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)     \
    X(vkEnumerateDeviceExtensionProperties)     \
    X(vkCreateDevice)                           \
    X(vkGetPhysicalDeviceFeatures2)             \
    X(vkGetDeviceProcAddr)

#define PANELSPUN_VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice)               \
    X(vkGetDeviceQueue)              \
    X(vkDeviceWaitIdle)              \
    X(vkCreateSwapchainKHR)          \
    X(vkDestroySwapchainKHR)         \
    X(vkGetSwapchainImagesKHR)       \
    X(vkAcquireNextImageKHR)         \
    X(vkQueuePresentKHR)             \
    X(vkQueueSubmit)                 \
    X(vkCreateCommandPool)           \
    X(vkDestroyCommandPool)          \
    X(vkAllocateCommandBuffers)      \
    X(vkBeginCommandBuffer)          \
    X(vkEndCommandBuffer)            \
    X(vkResetCommandBuffer)          \
    X(vkCreateFence)                 \
    X(vkDestroyFence)                \
    X(vkWaitForFences)               \
    X(vkResetFences)                 \
    X(vkCreateSemaphore)             \
    X(vkDestroySemaphore)            \
    X(vkCreateBuffer)                \
    X(vkDestroyBuffer)               \
    X(vkGetBufferMemoryRequirements) \
    X(vkCreateImage)                 \
    X(vkDestroyImage)                \
    X(vkGetImageMemoryRequirements)  \
    X(vkAllocateMemory)              \
    X(vkFreeMemory)                  \
    X(vkBindBufferMemory)            \
    X(vkBindImageMemory)             \
    X(vkMapMemory)                   \
    X(vkUnmapMemory)                 \
    X(vkCmdPipelineBarrier)          \
    X(vkCmdCopyBufferToImage)        \
    X(vkCmdCopyImage)                \
    X(vkCmdCopyImageToBuffer)

struct RegionRequest {
    Panel* panel = nullptr;
    Rect rect;
};

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct RegionImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class VulkanPresenter {
public:
    bool init(SDL_Window* window, bool validation, bool allFeatures, std::string* error);
    void shutdown();

    // Recreates the swapchain and the UI buffer when the size changed or presentation went stale.
    bool ensureSwapchain(int width, int height, std::string* error);
    void waitFrame();

    std::uint32_t* uiPixels() const { return static_cast<std::uint32_t*>(ui_.mapped); }
    std::uint32_t width() const { return extent_.width; }
    std::uint32_t height() const { return extent_.height; }
    bool stale() const { return stale_; }
    bool bgra() const { return format_ == VK_FORMAT_B8G8R8A8_UNORM; }

    // Presents the UI buffer with each region copied over it; returns false on a device error.
    bool present(const std::vector<RegionRequest>& regions, const std::string& capturePath, bool* captured);

    const VulkanContext& context() const { return ctx_; }
    int validationErrors() const { return validationErrors_; }
    int* validationErrorCounter() { return &validationErrors_; }

#define PANELSPUN_DECLARE(name) PFN_##name name = nullptr;
    PANELSPUN_VK_INSTANCE_FUNCS(PANELSPUN_DECLARE)
    PANELSPUN_VK_DEVICE_FUNCS(PANELSPUN_DECLARE)
#undef PANELSPUN_DECLARE

private:
    bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, HostBuffer& out);
    void destroyHostBuffer(HostBuffer& b);
    bool createRegionImage(std::uint32_t w, std::uint32_t h, RegionImage& out);
    void destroyRegionImage(RegionImage& r);
    std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const;
    void destroySwapchainResources();
    void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                 VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

    SDL_Window* window_ = nullptr;
    VulkanContext ctx_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{0, 0};
    bool canCapture_ = false;
    bool stale_ = true;
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> renderDone_;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    VkFence frameFence_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    HostBuffer ui_;
    HostBuffer readback_;
    std::map<Panel*, RegionImage> regions_;
    VkPhysicalDeviceMemoryProperties memProps_{};
    // Kept alive for VulkanContext::instanceInfo and deviceInfo.
    VkApplicationInfo appInfo_{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instanceInfo_{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    std::vector<const char*> instanceExtensions_;
    std::vector<const char*> layers_;
    float queuePriority_ = 1.0f;
    VkDeviceQueueCreateInfo queueInfo_{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    const char* deviceExtensions_[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures2 features_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features features11_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features features12_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features13_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkDeviceCreateInfo deviceInfo_{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    std::uint64_t frameIndex_ = 0;
    int validationErrors_ = 0;
};

}
