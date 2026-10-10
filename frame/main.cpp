// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// xr-pilot-frame: XR Pilot's client for a standalone Linux headset such as the Steam Frame. It joins an
// OXRSys runtime as a headset does, sends the headset's own head and controller tracking, decodes the
// PyroWave stream on the headset GPU and shows each eye through OpenXR (Vulkan). A frame is shown only
// with the head pose the runtime rendered it from (FrameSync); the compositor reprojects it from there.
//
//   xr-pilot-frame [--seconds N] [--snapshot left.png] [--probe]

#include "xrpilot/Client.h"
#include "xrpilot/FrameSync.h"
#include "xrpilot/GpuDecoder.h"
#include "xrpilot/Png.h"

#include <volk.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

namespace
{

std::atomic<bool> quit{false};

#define XR_CHECK(call)                                                                                      \
    do                                                                                                      \
    {                                                                                                       \
        const XrResult r_ = (call);                                                                         \
        if (XR_FAILED(r_))                                                                                  \
        {                                                                                                   \
            std::fprintf(stderr, "xr-pilot-frame: %s failed: %d\n", #call, int(r_));                       \
            return false;                                                                                   \
        }                                                                                                   \
    } while (0)

struct Options
{
    double seconds = 0.0; // 0 runs until a signal
    std::string snapshot;
    bool probe = false;
};

struct Eye
{
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkan2KHR> images;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Hand
{
    XrPath path = XR_NULL_PATH;
    XrSpace grip = XR_NULL_HANDLE;
};

class App
{
public:
    explicit App(const Options& options) : options_(options), client_([] {}) {}
    ~App() { shutdown(); }

    bool run();

private:
    bool createInstance();
    bool createVulkan();
    bool createSession();
    bool createSwapchains();
    bool createActions();
    void pollEvents();
    bool frame();
    void track(XrTime time, const XrView (&views)[2]);
    struct Slot;
    bool render(const Slot* slot);
    void shutdown();

    Options options_;
    xrpilot::Client client_;
    // Three decoded frames: one on screen, one ready, one being written, so decode never waits on the render.
    struct Slot
    {
        std::unique_ptr<xrpilot::GpuDecoder> decoder = std::make_unique<xrpilot::GpuDecoder>();
        float position[3] = {0.0f, 0.0f, 0.0f};
        XrQuaternionf orientation{0.0f, 0.0f, 0.0f, 1.0f};
    };
    std::array<Slot, 3> slots_;
    std::mutex slotMutex_;
    int readySlot_ = -1;
    int heldSlot_ = -1;
    xrpilot::FrameSync sync_; // decode thread only
    std::thread decodeThread_;
    std::atomic<bool> decoding_{false};
    void decodeLoop();
    // OpenXR may use the render queue inside its frame and swapchain calls; everything on it holds this.
    std::mutex queueMutex_;
    VkQueue decodeQueue_ = VK_NULL_HANDLE; // its own queue when the family has two, else the render queue

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    std::array<Eye, 2> eyes_;
    int64_t swapchainFormat_ = 0;
    std::string systemName_;

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction poseAction_ = XR_NULL_HANDLE;
    XrAction triggerAction_ = XR_NULL_HANDLE;
    XrAction squeezeAction_ = XR_NULL_HANDLE;
    XrAction stickAction_ = XR_NULL_HANDLE;
    XrAction stickClickAction_ = XR_NULL_HANDLE;
    XrAction lowerAction_ = XR_NULL_HANDLE; // A on the right, X on the left
    XrAction upperAction_ = XR_NULL_HANDLE; // B on the right, Y on the left
    XrAction menuAction_ = XR_NULL_HANDLE;
    std::array<Hand, 2> hands_;

    // Vulkan, created through XR_KHR_vulkan_enable2; the create infos stay alive for PyroWave.
    VkApplicationInfo appInfo_{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instanceInfo_{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDeviceVulkan13Features features13_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features features11_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 features_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    float priorities_[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo queueInfo_{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkDeviceCreateInfo deviceInfo_{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkInstance vkInstance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool decoderReady_ = false;

    // The frame on screen and the pose it was rendered from.
    float ipd_ = 0.064f;
    XrFovf sentFov_{};
    std::atomic<uint64_t> decoded_{0};
    std::atomic<uint64_t> decodeFailures_{0};
    std::atomic<uint64_t> decodeNs_{0};
    std::atomic<uint64_t> committed_{0};
    std::atomic<uint64_t> refused_{0};
    std::atomic<bool> snapshotWritten_{false};
    uint64_t shownNew_ = 0; // render thread: decoded frames put on screen
};

XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v)
{
    // v + 2w(u x v) + 2u x (u x v)
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f t{2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z), 2.0f * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

bool App::createInstance()
{
    const char* extensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(info.applicationInfo.applicationName, "xr-pilot-frame", XR_MAX_APPLICATION_NAME_SIZE - 1);
    info.applicationInfo.applicationVersion = 1;
    std::strncpy(info.applicationInfo.engineName, "xr-pilot", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    XR_CHECK(xrCreateInstance(&info, &instance_));

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    XR_CHECK(xrGetInstanceProperties(instance_, &properties));
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(instance_, &systemInfo, &system_));
    XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
    XR_CHECK(xrGetSystemProperties(instance_, system_, &system));
    systemName_ = system.systemName;
    std::printf("runtime %s %u.%u.%u, system '%s'\n", properties.runtimeName,
                XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion),
                XR_VERSION_PATCH(properties.runtimeVersion), system.systemName);
    return true;
}

bool App::createVulkan()
{
    PFN_xrGetVulkanGraphicsRequirements2KHR requirements2 = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR graphicsDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsRequirements2KHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&requirements2)));
    XR_CHECK(xrGetInstanceProcAddr(instance_, "xrCreateVulkanInstanceKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&createInstance)));
    XR_CHECK(xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsDevice2KHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&graphicsDevice)));
    XR_CHECK(xrGetInstanceProcAddr(instance_, "xrCreateVulkanDeviceKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&createDevice)));
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_CHECK(requirements2(instance_, system_, &requirements));

    if (volkInitialize() != VK_SUCCESS)
    {
        std::fprintf(stderr, "xr-pilot-frame: no Vulkan loader\n");
        return false;
    }
    appInfo_.pApplicationName = "xr-pilot-frame";
    appInfo_.apiVersion = VK_API_VERSION_1_3; // PyroWave needs 1.3
    instanceInfo_.pApplicationInfo = &appInfo_;
    XrVulkanInstanceCreateInfoKHR xrInstance{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstance.systemId = system_;
    xrInstance.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrInstance.vulkanCreateInfo = &instanceInfo_;
    VkResult vkResult = VK_SUCCESS;
    XR_CHECK(createInstance(instance_, &xrInstance, &vkInstance_, &vkResult));
    if (vkResult != VK_SUCCESS)
    {
        std::fprintf(stderr, "xr-pilot-frame: vkCreateInstance through OpenXR: %d\n", int(vkResult));
        return false;
    }
    volkLoadInstanceOnly(vkInstance_);

    XrVulkanGraphicsDeviceGetInfoKHR deviceGet{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGet.systemId = system_;
    deviceGet.vulkanInstance = vkInstance_;
    XR_CHECK(graphicsDevice(instance_, &deviceGet, &physical_));
    VkPhysicalDeviceProperties gpu{};
    vkGetPhysicalDeviceProperties(physical_, &gpu);
    std::printf("gpu %s, Vulkan %u.%u\n", gpu.deviceName, VK_API_VERSION_MAJOR(gpu.apiVersion),
                VK_API_VERSION_MINOR(gpu.apiVersion));

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, families.data());
    while (family_ < count && (families[family_].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
        ++family_;
    if (family_ == count)
    {
        std::fprintf(stderr, "xr-pilot-frame: no graphics queue\n");
        return false;
    }

    // Every supported core feature is enabled, which covers PyroWave's subgroup needs.
    features_.pNext = &features11_;
    features11_.pNext = &features12_;
    features12_.pNext = &features13_;
    vkGetPhysicalDeviceFeatures2(physical_, &features_);
    features_.features.robustBufferAccess = VK_FALSE;
    queueInfo_.queueFamilyIndex = family_;
    queueInfo_.queueCount = std::min(2u, families[family_].queueCount);
    queueInfo_.pQueuePriorities = priorities_;
    deviceInfo_.pNext = &features_;
    deviceInfo_.queueCreateInfoCount = 1;
    deviceInfo_.pQueueCreateInfos = &queueInfo_;
    XrVulkanDeviceCreateInfoKHR xrDevice{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDevice.systemId = system_;
    xrDevice.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrDevice.vulkanPhysicalDevice = physical_;
    xrDevice.vulkanCreateInfo = &deviceInfo_;
    XR_CHECK(createDevice(instance_, &xrDevice, &device_, &vkResult));
    if (vkResult != VK_SUCCESS)
    {
        std::fprintf(stderr, "xr-pilot-frame: vkCreateDevice through OpenXR: %d\n", int(vkResult));
        return false;
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, family_, 0, &queue_);
    decodeQueue_ = queue_;
    if (queueInfo_.queueCount > 1)
        vkGetDeviceQueue(device_, family_, 1, &decodeQueue_);
    std::printf("decode queue: %s\n", decodeQueue_ != queue_ ? "its own" : "shared with the render");

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family_;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS ||
        vkCreateFence(device_, &fenceInfo, nullptr, &fence_) != VK_SUCCESS)
        return false;
    cmdInfo.commandPool = pool_;
    return vkAllocateCommandBuffers(device_, &cmdInfo, &cmd_) == VK_SUCCESS;
}

bool App::createSession()
{
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vkInstance_;
    binding.physicalDevice = physical_;
    binding.device = device_;
    binding.queueFamilyIndex = family_;
    binding.queueIndex = 0;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = system_;
    XR_CHECK(xrCreateSession(instance_, &info, &session_));

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_FAILED(xrCreateReferenceSpace(session_, &spaceInfo, &space_)))
    {
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XR_CHECK(xrCreateReferenceSpace(session_, &spaceInfo, &space_));
    }
    std::printf("space %s\n", spaceInfo.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage" : "local");
    return true;
}

bool App::createSwapchains()
{
    uint32_t count = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr));
    std::vector<int64_t> formats(count);
    XR_CHECK(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()));
    // An sRGB swapchain with an sRGB decoded frame keeps the stream's bytes through the blit.
    const int64_t wanted[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_FORMAT_B8G8R8A8_UNORM};
    for (int64_t w : wanted)
    {
        if (std::find(formats.begin(), formats.end(), w) != formats.end())
        {
            swapchainFormat_ = w;
            break;
        }
    }
    if (swapchainFormat_ == 0)
    {
        std::fprintf(stderr, "xr-pilot-frame: no 8-bit RGBA swapchain format\n");
        return false;
    }
    XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    XR_CHECK(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                               &count, views));
    for (uint32_t e = 0; e < 2; ++e)
    {
        Eye& eye = eyes_[e];
        eye.width = views[e].recommendedImageRectWidth;
        eye.height = views[e].recommendedImageRectHeight;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = swapchainFormat_;
        info.sampleCount = 1;
        info.width = eye.width;
        info.height = eye.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(session_, &info, &eye.swapchain));
        XR_CHECK(xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr));
        eye.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(eye.swapchain, count, &count,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())));
    }
    std::printf("eyes %ux%u, swapchain format %lld\n", eyes_[0].width, eyes_[0].height,
                static_cast<long long>(swapchainFormat_));
    return true;
}

bool App::createActions()
{
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "pilot");
    std::strcpy(setInfo.localizedActionSetName, "Pilot");
    XR_CHECK(xrCreateActionSet(instance_, &setInfo, &actionSet_));
    XR_CHECK(xrStringToPath(instance_, "/user/hand/left", &hands_[0].path));
    XR_CHECK(xrStringToPath(instance_, "/user/hand/right", &hands_[1].path));
    const XrPath both[2] = {hands_[0].path, hands_[1].path};
    const auto make = [&](XrAction& action, const char* name, XrActionType type) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(info.actionName, name);
        std::strcpy(info.localizedActionName, name);
        info.actionType = type;
        info.countSubactionPaths = 2;
        info.subactionPaths = both;
        return xrCreateAction(actionSet_, &info, &action);
    };
    XR_CHECK(make(poseAction_, "grip", XR_ACTION_TYPE_POSE_INPUT));
    XR_CHECK(make(triggerAction_, "trigger", XR_ACTION_TYPE_FLOAT_INPUT));
    XR_CHECK(make(squeezeAction_, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT));
    XR_CHECK(make(stickAction_, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT));
    XR_CHECK(make(stickClickAction_, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT));
    XR_CHECK(make(lowerAction_, "lower_button", XR_ACTION_TYPE_BOOLEAN_INPUT));
    XR_CHECK(make(upperAction_, "upper_button", XR_ACTION_TYPE_BOOLEAN_INPUT));
    XR_CHECK(make(menuAction_, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT));

    struct Binding
    {
        XrAction action;
        std::string path;
    };
    const auto suggest = [&](const char* profile, const std::vector<Binding>& bindings) {
        std::vector<XrActionSuggestedBinding> suggested;
        for (const Binding& b : bindings)
        {
            XrPath path = XR_NULL_PATH;
            if (XR_SUCCEEDED(xrStringToPath(instance_, b.path.c_str(), &path)))
                suggested.push_back({b.action, path});
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        if (XR_FAILED(xrStringToPath(instance_, profile, &info.interactionProfile)))
            return;
        info.countSuggestedBindings = uint32_t(suggested.size());
        info.suggestedBindings = suggested.data();
        const XrResult r = xrSuggestInteractionProfileBindings(instance_, &info);
        std::printf("bindings %s: %s\n", profile, XR_SUCCEEDED(r) ? "accepted" : "refused");
    };
    std::vector<Binding> common;
    for (const char* side : {"left", "right"})
    {
        const std::string h = std::string("/user/hand/") + side + "/input/";
        common.push_back({poseAction_, h + "grip/pose"});
        common.push_back({triggerAction_, h + "trigger/value"});
        common.push_back({squeezeAction_, h + "squeeze/value"});
        common.push_back({stickAction_, h + "thumbstick"});
        common.push_back({stickClickAction_, h + "thumbstick/click"});
    }
    std::vector<Binding> touch = common;
    touch.insert(touch.end(), {{lowerAction_, "/user/hand/left/input/x/click"},
                               {upperAction_, "/user/hand/left/input/y/click"},
                               {lowerAction_, "/user/hand/right/input/a/click"},
                               {upperAction_, "/user/hand/right/input/b/click"},
                               {menuAction_, "/user/hand/left/input/menu/click"}});
    suggest("/interaction_profiles/oculus/touch_controller", touch);
    std::vector<Binding> index = common;
    index.insert(index.end(), {{lowerAction_, "/user/hand/left/input/a/click"},
                               {upperAction_, "/user/hand/left/input/b/click"},
                               {lowerAction_, "/user/hand/right/input/a/click"},
                               {upperAction_, "/user/hand/right/input/b/click"}});
    suggest("/interaction_profiles/valve/index_controller", index);
    suggest("/interaction_profiles/khr/simple_controller",
            {{poseAction_, "/user/hand/left/input/grip/pose"},
             {poseAction_, "/user/hand/right/input/grip/pose"},
             {triggerAction_, "/user/hand/left/input/select/click"},
             {triggerAction_, "/user/hand/right/input/select/click"},
             {menuAction_, "/user/hand/left/input/menu/click"},
             {menuAction_, "/user/hand/right/input/menu/click"}});

    for (Hand& hand : hands_)
    {
        XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        info.action = poseAction_;
        info.subactionPath = hand.path;
        info.poseInActionSpace.orientation.w = 1.0f;
        XR_CHECK(xrCreateActionSpace(session_, &info, &hand.grip));
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    XR_CHECK(xrAttachSessionActionSets(session_, &attach));
    return true;
}

void App::pollEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            state_ = changed.state;
            std::printf("session state %d\n", int(state_));
            if (state_ == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running_ = XR_SUCCEEDED(xrBeginSession(session_, &begin));
            }
            else if (state_ == XR_SESSION_STATE_STOPPING)
            {
                xrEndSession(session_);
                running_ = false;
            }
            else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING)
            {
                quit = true;
            }
        }
        else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        {
            quit = true;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// The headset's head, hands and inputs become the tracking the client streams to the runtime.
void App::track(XrTime time, const XrView (&views)[2])
{
    const XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    xrSyncActions(session_, &syncInfo);

    const auto floatOf = [&](XrAction action, XrPath hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(xrGetActionStateFloat(session_, &info, &state)) && state.isActive ? state.currentState
                                                                                                : 0.0f;
    };
    const auto boolOf = [&](XrAction action, XrPath hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &info, &state)) && state.isActive &&
               state.currentState == XR_TRUE;
    };

    const XrVector3f& l = views[0].pose.position;
    const XrVector3f& r = views[1].pose.position;
    const float dx = r.x - l.x;
    const float dy = r.y - l.y;
    const float dz = r.z - l.z;
    const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
    ipd_ = std::isfinite(ipd) && ipd > 0.03f && ipd < 0.09f ? ipd : 0.064f;
    // The runtime renders a symmetric field of view; take the widest half angles the panel shows.
    const XrFovf& f = views[0].fov;
    const float halfH = std::max(std::fabs(f.angleLeft), std::fabs(f.angleRight));
    const float halfV = std::max(std::fabs(f.angleUp), std::fabs(f.angleDown));

    xrpilot::HandState handStates[2];
    uint32_t buttons = 0;
    for (int h = 0; h < 2; ++h)
    {
        xrpilot::HandState& hand = handStates[h];
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const bool located = XR_SUCCEEDED(xrLocateSpace(hands_[h].grip, space_, time, &location)) &&
                             (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                             (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        hand.present = located;
        hand.manual = true;
        if (located)
        {
            hand.pose.position[0] = location.pose.position.x;
            hand.pose.position[1] = location.pose.position.y;
            hand.pose.position[2] = location.pose.position.z;
            const float q[4] = {location.pose.orientation.x, location.pose.orientation.y,
                                location.pose.orientation.z, location.pose.orientation.w};
            xrpilot::fromQuaternion(q, hand.pose.rotation);
        }
        hand.trigger = floatOf(triggerAction_, hands_[h].path);
        hand.grip = floatOf(squeezeAction_, hands_[h].path);
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = stickAction_;
        info.subactionPath = hands_[h].path;
        XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &info, &stick)) && stick.isActive)
        {
            hand.stick[0] = stick.currentState.x;
            hand.stick[1] = stick.currentState.y;
        }
        using namespace oxr::protocol;
        if (boolOf(lowerAction_, hands_[h].path))
            buttons |= h == 0 ? BUTTON_X : BUTTON_A;
        if (boolOf(upperAction_, hands_[h].path))
            buttons |= h == 0 ? BUTTON_Y : BUTTON_B;
        if (boolOf(stickClickAction_, hands_[h].path))
            buttons |= h == 0 ? BUTTON_LEFT_THUMBSTICK : BUTTON_RIGHT_THUMBSTICK;
        if (boolOf(menuAction_, hands_[h].path))
            buttons |= BUTTON_MENU;
    }

    client_.updateAgent([&](xrpilot::AgentState& agent) {
        agent.head.position[0] = (l.x + r.x) * 0.5f;
        agent.head.position[1] = (l.y + r.y) * 0.5f;
        agent.head.position[2] = (l.z + r.z) * 0.5f;
        const XrQuaternionf& o = views[0].pose.orientation;
        const float q[4] = {o.x, o.y, o.z, o.w};
        xrpilot::fromQuaternion(q, agent.head.rotation);
        agent.ipd = ipd_;
        agent.verticalFovDegrees = 2.0f * halfV * 57.29577951f;
        agent.eyeAspect = std::tan(halfH) / std::max(std::tan(halfV), 1e-3f);
        agent.hands[0] = handStates[0];
        agent.hands[1] = handStates[1];
        agent.buttons = buttons;
        agent.seated = false;
        float h = 0.0f;
        float v = 0.0f;
        xrpilot::eyeHalfFov(agent, h, v);
        const float* t = agent.renderTangents;
        if (t[1] > t[0] && t[2] > t[3])
            sentFov_ = {std::atan(t[0]), std::atan(t[1]), std::atan(t[2]), std::atan(t[3])};
        else
            sentFov_ = {-h, h, v, -v};
    });
}

// Both eyes in one submit; the runtime's swapchain calls and the submit hold the queue lock.
bool App::render(const Slot* slot)
{
    uint32_t index[2] = {0, 0};
    VkImage images[2] = {};
    for (uint32_t e = 0; e < 2; ++e)
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XR_CHECK(xrAcquireSwapchainImage(eyes_[e].swapchain, &acquire, &index[e]));
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        XR_CHECK(xrWaitSwapchainImage(eyes_[e].swapchain, &wait));
        images[e] = eyes_[e].images[index[e]].image;
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd_, 0);
    vkBeginCommandBuffer(cmd_, &begin);
    for (uint32_t e = 0; e < 2; ++e)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[e];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
        if (slot != nullptr)
        {
            slot->decoder->recordEye(cmd_, int(e), images[e], eyes_[e].width, eyes_[e].height);
        }
        else
        {
            const VkClearColorValue standby = {{0.015f, 0.02f, 0.03f, 1.0f}};
            vkCmdClearColorImage(cmd_, images[e], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &standby, 1,
                                 &barrier.subresourceRange);
        }
        // OpenXR takes the image back in COLOR_ATTACHMENT_OPTIMAL.
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
    }
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    vkResetFences(device_, 1, &fence_);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        vkQueueSubmit(queue_, 1, &submit, fence_);
    }
    // The slot stays held until a newer one replaces it, so the decoder never writes what this reads.
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, 1'000'000'000ull);

    for (uint32_t e = 0; e < 2; ++e)
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XR_CHECK(xrReleaseSwapchainImage(eyes_[e].swapchain, &release));
    }
    return true;
}

// Takes frames from the network, keeps only those with their own render pose, and decodes each into a
// slot neither on screen nor ready; the render loop never waits for a decode.
void App::decodeLoop()
{
    while (decoding_)
    {
        const int64_t now = xrpilot::monotonicNowNs();
        std::optional<AssembledVideoFrame> next = sync_.next(
            client_.takeFrame(), [&](AssembledVideoFrame& f) { return client_.attachRenderPose(f); }, now);
        committed_ = sync_.committed();
        refused_ = sync_.refused();
        if (!next)
        {
            usleep(500);
            continue;
        }
        int w = 0;
        {
            std::lock_guard<std::mutex> lock(slotMutex_);
            while (w == readySlot_ || w == heldSlot_)
                ++w;
        }
        Slot& slot = slots_[w];
        bool ok = false;
        {
            std::unique_lock<std::mutex> lock(queueMutex_, std::defer_lock);
            if (decodeQueue_ == queue_)
                lock.lock();
            ok = slot.decoder->decode(next->nalUnit.data(), next->nalUnit.size());
        }
        const int64_t done = xrpilot::monotonicNowNs();
        if (!ok)
        {
            ++decodeFailures_;
            client_.requestKeyframe();
            continue;
        }
        client_.reportLatency(*next, now, done);
        decodeNs_ += uint64_t(done - now);
        std::copy(std::begin(next->renderPosition), std::end(next->renderPosition), std::begin(slot.position));
        slot.orientation = {next->renderOrientation[0], next->renderOrientation[1], next->renderOrientation[2],
                            next->renderOrientation[3]};
        {
            std::lock_guard<std::mutex> lock(slotMutex_);
            readySlot_ = w;
        }
        const uint64_t decoded = ++decoded_;
        if (!options_.snapshot.empty() && !snapshotWritten_ && decoded >= 30)
        {
            std::vector<uint8_t> rgba;
            int sw = 0;
            int sh = 0;
            bool written = false;
            {
                std::unique_lock<std::mutex> lock(queueMutex_, std::defer_lock);
                if (decodeQueue_ == queue_)
                    lock.lock();
                written = slot.decoder->snapshotLeftEye(rgba, sw, sh);
            }
            written = written && xrpilot::writePng(options_.snapshot, rgba.data(), sw, sh);
            snapshotWritten_ = written;
            std::printf("snapshot %s %dx%d: %s\n", options_.snapshot.c_str(), sw, sh, written ? "written" : "failed");
        }
    }
}

bool App::frame()
{
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XR_CHECK(xrWaitFrame(session_, &waitInfo, &frameState));
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        XR_CHECK(xrBeginFrame(session_, &beginInfo));
    }

    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frameState.predictedDisplayTime;
    locate.space = space_;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    const bool located = XR_SUCCEEDED(xrLocateViews(session_, &locate, &viewState, 2, &count, views)) &&
                         (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    if (located)
        track(frameState.predictedDisplayTime, views);

    // The newest decoded frame, which always carries its own render pose (FrameSync).
    {
        std::lock_guard<std::mutex> lock(slotMutex_);
        if (readySlot_ >= 0)
        {
            heldSlot_ = readySlot_;
            readySlot_ = -1;
            ++shownNew_;
        }
    }
    const Slot* slot = heldSlot_ >= 0 ? &slots_[heldSlot_] : nullptr;

    XrCompositionLayerProjectionView projection[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                   {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (frameState.shouldRender && located)
    {
        if (!render(slot))
            return false;
        for (uint32_t e = 0; e < 2; ++e)
        {
            XrCompositionLayerProjectionView& view = projection[e];
            if (slot != nullptr)
            {
                // The eye the runtime rendered: its head pose, half the eye distance to the side, its field of view.
                const XrVector3f offset =
                    rotate(slot->orientation, {e == 0 ? -0.5f * ipd_ : 0.5f * ipd_, 0.0f, 0.0f});
                view.pose.orientation = slot->orientation;
                view.pose.position = {slot->position[0] + offset.x, slot->position[1] + offset.y,
                                      slot->position[2] + offset.z};
                view.fov = sentFov_;
                if (e == 1) // the right eye mirrors the left
                    view.fov = {-sentFov_.angleRight, -sentFov_.angleLeft, sentFov_.angleUp, sentFov_.angleDown};
            }
            else
            {
                view.pose = views[e].pose;
                view.fov = views[e].fov;
            }
            view.subImage.swapchain = eyes_[e].swapchain;
            view.subImage.imageRect = {{0, 0}, {int32_t(eyes_[e].width), int32_t(eyes_[e].height)}};
        }
        layer.space = space_;
        layer.viewCount = 2;
        layer.views = projection;
        endInfo.layerCount = 1;
        endInfo.layers = layers;
    }
    std::lock_guard<std::mutex> lock(queueMutex_);
    XR_CHECK(xrEndFrame(session_, &endInfo));
    return true;
}

bool App::run()
{
    if (!createInstance() || !createVulkan() || !createSession() || !createSwapchains() || !createActions())
        return false;
    if (options_.probe)
        return true;

    xrpilot::GpuContext gpu;
    gpu.getInstanceProcAddr = vkGetInstanceProcAddr;
    gpu.instance = vkInstance_;
    gpu.physicalDevice = physical_;
    gpu.device = device_;
    gpu.queue = decodeQueue_;
    gpu.queueFamily = family_;
    gpu.instanceInfo = &instanceInfo_;
    gpu.deviceInfo = &deviceInfo_;
    gpu.frameFormat = swapchainFormat_ == VK_FORMAT_R8G8B8A8_SRGB || swapchainFormat_ == VK_FORMAT_B8G8R8A8_SRGB
                          ? VK_FORMAT_R8G8B8A8_SRGB
                          : VK_FORMAT_R8G8B8A8_UNORM;
    std::string error;
    decoderReady_ = true;
    for (Slot& slot : slots_)
        decoderReady_ = decoderReady_ && slot.decoder->initialize(gpu, &error);
    if (!decoderReady_)
    {
        std::fprintf(stderr, "xr-pilot-frame: PyroWave decoder: %s\n", error.c_str());
        return false;
    }
    if (!client_.start(&error))
    {
        std::fprintf(stderr, "xr-pilot-frame: client: %s\n", error.c_str());
        return false;
    }

    decoding_ = true;
    decodeThread_ = std::thread([this] { decodeLoop(); });

    const int64_t start = xrpilot::monotonicNowNs();
    int64_t lastReport = start;
    uint64_t lastShown = 0;
    uint64_t lastDecoded = 0;
    uint64_t lastDecodeNs = 0;
    uint64_t renderFrames = 0;
    uint64_t lastRenderFrames = 0;
    while (!quit)
    {
        pollEvents();
        if (running_)
        {
            if (!frame())
                return false;
            ++renderFrames;
        }
        if (!running_)
            usleep(10'000);
        const int64_t now = xrpilot::monotonicNowNs();
        if (now - lastReport >= 1'000'000'000)
        {
            lastReport = now;
            const xrpilot::ClientStatus s = client_.status();
            const uint64_t decoded = decoded_;
            const uint64_t decodeNs = decodeNs_;
            const double decodeMs =
                decoded > lastDecoded ? double(decodeNs - lastDecodeNs) / double(decoded - lastDecoded) / 1e6 : 0.0;
            std::printf("state %d connected %d server %s frames %llu decoded %llu committed %llu refused %llu "
                        "shown/s %llu render/s %llu decode-ms %.2f "
                        "dropped %llu fec %llu decode-failures %llu tracking %llu\n",
                        int(state_), s.connected ? 1 : 0, s.server.c_str(),
                        static_cast<unsigned long long>(s.framesAssembled), static_cast<unsigned long long>(decoded),
                        static_cast<unsigned long long>(committed_.load()),
                        static_cast<unsigned long long>(refused_.load()),
                        static_cast<unsigned long long>(shownNew_ - lastShown),
                        static_cast<unsigned long long>(renderFrames - lastRenderFrames), decodeMs,
                        static_cast<unsigned long long>(s.framesDropped),
                        static_cast<unsigned long long>(s.fecRecoveries),
                        static_cast<unsigned long long>(decodeFailures_.load()),
                        static_cast<unsigned long long>(s.trackingSent));
            std::fflush(stdout);
            lastShown = shownNew_;
            lastDecoded = decoded;
            lastDecodeNs = decodeNs;
            lastRenderFrames = renderFrames;
        }
        if (options_.seconds > 0.0 && double(now - start) * 1e-9 >= options_.seconds)
            break;
    }
    if (!options_.snapshot.empty() && !snapshotWritten_)
    {
        std::fprintf(stderr, "xr-pilot-frame: no snapshot: %llu frames decoded\n",
                     static_cast<unsigned long long>(decoded_.load()));
        return false;
    }
    return true;
}

void App::shutdown()
{
    decoding_ = false;
    if (decodeThread_.joinable())
        decodeThread_.join();
    client_.stop();
    if (device_ != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device_);
    for (Slot& slot : slots_)
        slot.decoder.reset(); // before the device it decodes on
    for (Eye& eye : eyes_)
    {
        if (eye.swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(eye.swapchain);
        eye.swapchain = XR_NULL_HANDLE;
    }
    for (Hand& hand : hands_)
    {
        if (hand.grip != XR_NULL_HANDLE)
            xrDestroySpace(hand.grip);
        hand.grip = XR_NULL_HANDLE;
    }
    if (actionSet_ != XR_NULL_HANDLE)
        xrDestroyActionSet(actionSet_);
    actionSet_ = XR_NULL_HANDLE;
    if (space_ != XR_NULL_HANDLE)
        xrDestroySpace(space_);
    space_ = XR_NULL_HANDLE;
    if (session_ != XR_NULL_HANDLE)
        xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
    if (device_ != VK_NULL_HANDLE)
    {
        vkDestroyFence(device_, fence_, nullptr);
        vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (vkInstance_ != VK_NULL_HANDLE)
        vkDestroyInstance(vkInstance_, nullptr);
    vkInstance_ = VK_NULL_HANDLE;
    if (instance_ != XR_NULL_HANDLE)
        xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc)
            options.seconds = std::atof(argv[++i]);
        else if (arg == "--snapshot" && i + 1 < argc)
            options.snapshot = argv[++i];
        else if (arg == "--probe")
            options.probe = true;
        else
        {
            std::fprintf(stderr, "usage: xr-pilot-frame [--seconds N] [--snapshot left.png] [--probe]\n");
            return 2;
        }
    }
    std::signal(SIGINT, [](int) { quit = true; });
    std::signal(SIGTERM, [](int) { quit = true; });
    App app(options);
    const bool ok = app.run();
    return ok ? 0 : 1;
}
