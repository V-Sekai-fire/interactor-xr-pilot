// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <volk.h>

// XR_USE_GRAPHICS_API_VULKAN comes from CMake, with XR_USE_PLATFORM_ANDROID.
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <vector>

#include "VideoDecoder.h"

namespace oxr
{

// Draws the decoded video and the local shell into each eye's swapchain image, one eye at a time.
// Device calls go through its own volk table, since the decoder points volk's globals at its own device.
class VulkanRenderer
{
  public:
    struct ShellVertex
    {
        float x, y, z;
        float r, g, b, a;
    };

    // The blit shader's push constants, in blit.frag's order.
    struct BlitParams
    {
        float eyeSourceMin[2] = {0.0f, 0.0f};
        float eyeSourceMax[2] = {1.0f, 1.0f};
        float logicalTexelSize[2] = {0.0f, 0.0f};
        float foveationCenterSize[2] = {1.0f, 1.0f};
        float foveationCenterShift[2] = {0.0f, 0.0f};
        float foveationEdgeRatio[2] = {1.0f, 1.0f};
        float foveationEyeSizeRatio[2] = {1.0f, 1.0f};
        float reprojectionWarpOffset[2] = {0.0f, 0.0f};
        float upscaleEdgeThreshold = 4.0f / 255.0f;
        float upscaleSharpness = 2.0f;
        int32_t foveatedEncodingEnabled = 0;
        int32_t clientUpscalingEnabled = 0;
        int32_t reprojectionWarpEnabled = 0;
        int32_t passthroughAlphaEnabled = 0;
    };

    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    // Creates the Vulkan instance and device through XR_KHR_vulkan_enable2, before the session.
    bool CreateDevice(XrInstance instance, XrSystemId systemId);
    const XrGraphicsBindingVulkan2KHR& Binding() const
    {
        return binding_;
    }

    int64_t ChooseSwapchainFormat(XrSession session) const;

    bool AdoptSwapchain(int eye, XrSwapchain swapchain, int64_t format, uint32_t width, uint32_t height);

    bool UploadPlanes(const VideoDecoder::DecodedFrame& frame);

    void BeginEye(int eye, uint32_t imageIndex, const float clearColour[4]);
    // GL's glViewport: x and y from the image's bottom-left corner.
    void SetViewport(int32_t x, int32_t y, uint32_t width, uint32_t height);
    void DrawVideo(const BlitParams& params);
    void DrawShell(const float mvp[16], const std::vector<ShellVertex>& triangles,
                   const std::vector<ShellVertex>& lines);
    // Submits the eye's commands; the image can be released after.
    void EndEye();

    void ReleaseSwapchains();
    void Shutdown();

  private:
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    struct Plane
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct Eye
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> framebuffers;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false;
        Buffer shellVertices;
    };

    bool CreatePipelines(VkFormat format);
    VkPipeline CreatePipeline(VkShaderModule vertex, VkShaderModule fragment, VkPipelineLayout layout,
                              VkPrimitiveTopology topology, bool shellVertices, bool blend);
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer);
    void DestroyBuffer(Buffer* buffer);
    bool CreatePlane(uint32_t width, uint32_t height, Plane* plane);
    void DestroyPlane(Plane* plane);
    int32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    void WaitEye(Eye& eye);
    void ReleaseEye(Eye& eye);

    XrGraphicsBindingVulkan2KHR binding_ = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VolkDeviceTable vk_ = {};
    PFN_vkDestroyInstance destroyInstance_ = nullptr;
    VkPhysicalDeviceMemoryProperties memoryProperties_ = {};

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout planeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet planeSet_ = VK_NULL_HANDLE;
    VkPipelineLayout blitLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout shellLayout_ = VK_NULL_HANDLE;
    VkPipeline blitPipeline_ = VK_NULL_HANDLE;
    VkPipeline shellTrianglePipeline_ = VK_NULL_HANDLE;
    VkPipeline shellLinePipeline_ = VK_NULL_HANDLE;

    Eye eyes_[2];
    int currentEye_ = -1;

    Plane planes_[VideoDecoder::PlaneCount];
    Buffer staging_;
    VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;
    bool uploadSubmitted_ = false;
};

} // namespace oxr
