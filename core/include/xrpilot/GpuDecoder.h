// SPDX-License-Identifier: MPL-2.0
//
// PyroWave decode on a device the caller owns, ported from oxrsys PyroWaveDecoder: PyroWave decodes
// into three plane images and the Lean-authored yuv420_to_rgbx kernel packs them, all on the GPU.
// Frames reach the CPU only through snapshotLeftEye.

#pragma once

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xrpilot
{

struct GpuContext
{
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    const VkInstanceCreateInfo* instanceInfo = nullptr;
    const VkDeviceCreateInfo* deviceInfo = nullptr;
};

class GpuDecoder final
{
public:
    GpuDecoder();
    ~GpuDecoder();

    GpuDecoder(const GpuDecoder&) = delete;
    GpuDecoder& operator=(const GpuDecoder&) = delete;

    bool initialize(const GpuContext& context, std::string* error);

    // Decodes one stream frame; submits on the context's queue and waits.
    bool decode(const uint8_t* data, size_t size);

    bool hasFrame() const;
    int width() const;
    int height() const;

    // Records a letterboxed copy of the left eye into target, which is in TRANSFER_DST_OPTIMAL and stays so.
    // With reticle, two short white bars cross at the centre of the eye, where gaze and pointing aim.
    void recordLeftEye(VkCommandBuffer commands, VkImage target, uint32_t targetWidth, uint32_t targetHeight,
                       bool reticle = false);

    // The left eye of the last decoded frame, read back from the GPU as tightly packed RGBA8.
    bool snapshotLeftEye(std::vector<uint8_t>& rgba, int& width, int& height);

    // Width and height from the frame's sequence header, or false without one.
    static bool frameSize(const uint8_t* data, size_t size, int& width, int& height);

private:
    struct Gpu;
    std::unique_ptr<Gpu> gpu_;
};

} // namespace xrpilot
