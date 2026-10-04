// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace panelspun {

// The window's Vulkan objects, for a consumer that records its own work on the same device.
struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    std::uint32_t apiVersion = 0;
    // How the instance and device were created, for libraries that wrap an existing device and must
    // know its extensions and features. Valid for the window's lifetime.
    const VkInstanceCreateInfo* instanceInfo = nullptr;
    const VkDeviceCreateInfo* deviceInfo = nullptr;
};

// One frame of a panel's Vulkan region. image is in TRANSFER_DST_OPTIMAL on entry and must be
// left in that layout; the window copies it into the swapchain at the panel's content rect.
struct VulkanRegionFrame {
    const VulkanContext* context = nullptr;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frameIndex = 0;
};

}
