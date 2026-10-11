// SPDX-License-Identifier: MPL-2.0
//
// Linux Vulkan interop. At xrEndFrame the app's queue copies both released eyes side by side
// into a packed image of the runtime's, then signals a timeline semaphore. The packed image's
// memory and the semaphore are exported as opaque file descriptors, which the PyroWave encoder
// (PyroWaveVideoEncoderLinux.cpp) imports into its own VkDevice on the same GPU. The frame never
// leaves the GPU, and the copy keeps sRGB-encoded bytes as they are.

#pragma once

#if defined(__linux__) && defined(XR_USE_GRAPHICS_API_VULKAN)

#include "GraphicsTypes.h"

#include <cstdint>
#include <functional>
#include <memory>

#include <vulkan/vulkan.h>

// Instance and device extensions the app must enable for the export.
extern const char* const kLinuxVulkanInstanceExtensions;
extern const char* const kLinuxVulkanDeviceExtensions;

// The session's interop: the app's queue, the exported timeline semaphore and the packed
// images. Empty on failure (logged); the session then streams nothing.
std::shared_ptr<void> LinuxCreateVulkanInterop(const VulkanGraphicsContext& context);

// One released eye, as a swapchain hands it to xrEndFrame; valid until xrEndFrame returns.
struct LinuxEyeSlice
{
    VkImage image = VK_NULL_HANDLE; // in COLOR_ATTACHMENT_OPTIMAL, as the app released it
    uint32_t arrayIndex = 0;
    int32_t x = 0; // the view's imageRect within the slice
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

// Replace both eye slices of frameSource with one packed image, copied on the app's queue,
// which is externally synchronized during xrEndFrame. Leaves frameSource empty when the
// encoder still holds every packed image, or for formats the encoder does not take.
void LinuxPackVulkanFrame(const std::shared_ptr<void>& interop, FrameSource& frameSource);

// The packed eyes, handed to the encoder as FrameImageSource.image of both eyes.
struct LinuxPackedImage
{
    uint64_t imageId = 0; // new for each packed image the interop creates
    VkImageCreateInfo createInfo = {}; // as exported; pNext is null
    std::function<int()> exportMemory; // a new opaque fd of the image's memory, or -1

    uint64_t semaphoreId = 0; // new for each interop
    std::function<int()> exportSemaphore; // a new opaque fd of the timeline semaphore, or -1
    uint64_t waitValue = 0; // the semaphore reaches it once the copy has landed
};

#endif
