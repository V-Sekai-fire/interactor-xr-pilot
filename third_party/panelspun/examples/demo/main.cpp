// SPDX-License-Identifier: Apache-2.0 OR MIT
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "panelspun/widgets.h"
#include "panelspun/window.h"

using namespace panelspun;

namespace {

constexpr std::uint8_t kVideoRgb[3] = {26, 115, 140};

// Stands in for a decoder's output: clears its region with a solid colour on the GPU.
class VideoPanel : public Panel {
public:
    VideoPanel(bool vulkan, std::atomic<int>* frames, std::atomic<int>* updates)
        : Panel("Video preview"), vulkan_(vulkan), frames_(frames), updates_(updates) {}
    bool usesVulkanRegion() const override { return vulkan_; }
    void draw(DrawContext& ctx) override {
        (void)ctx;
        ++*frames_;
    }
    void recordVulkan(const VulkanRegionFrame& f) override {
        ++*frames_;
        if (!clear_)
            clear_ = reinterpret_cast<PFN_vkCmdClearColorImage>(
                f.context->getDeviceProcAddr(f.context->device, "vkCmdClearColorImage"));
        VkClearColorValue color{};
        color.float32[0] = kVideoRgb[0] / 255.0f;
        color.float32[1] = kVideoRgb[1] / 255.0f;
        color.float32[2] = kVideoRgb[2] / 255.0f;
        color.float32[3] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        clear_(f.commands, f.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    }
    void update(const VulkanContext& context) override {
        (void)context;
        ++*updates_;
    }

private:
    bool vulkan_;
    std::atomic<int>* frames_;
    std::atomic<int>* updates_;
    PFN_vkCmdClearColorImage clear_ = nullptr;
};

bool pixelMatches(SDL_Surface* s, int x, int y, const std::uint8_t rgb[3]) {
    Uint8 r = 0, g = 0, b = 0, a = 0;
    if (!SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a)) return false;
    return std::abs(r - rgb[0]) <= 1 && std::abs(g - rgb[1]) <= 1 && std::abs(b - rgb[2]) <= 1;
}

// The video region must show the Vulkan clear colour and the controls panel must not.
int checkCapture(const std::string& path, SplitTree tree) {
    SDL_Surface* s = SDL_LoadBMP(path.c_str());
    if (!s) {
        std::fprintf(stderr, "FAIL could not read %s: %s\n", path.c_str(), SDL_GetError());
        return 3;
    }
    std::vector<LeafRect> leaves = tree.layout(Rect{0, 0, s->w, s->h});
    int failures = 0;
    for (const LeafRect& l : leaves) {
        int x = l.rect.x + l.rect.w / 2;
        int y = l.rect.y + l.rect.h - 8;
        bool isVideo = pixelMatches(s, x, y, kVideoRgb);
        bool want = l.id == "video";
        std::printf("%s pixel (%d,%d) in '%s' %s the Vulkan clear colour\n", isVideo == want ? "PASS" : "FAIL", x, y,
                    l.id.c_str(), isVideo ? "is" : "is not");
        if (isVideo != want) ++failures;
    }
    SDL_DestroySurface(s);
    return failures == 0 ? 0 : 3;
}

}

int main(int argc, char** argv) {
    int frames = 0;
    std::string screenshot;
    bool validate = false;
    bool vulkanRegion = true;
    bool check = false;
    bool allFeatures = false;
    bool checkFeatures = false;
    int wakes = -1;
    bool sendWakes = true;
    bool minimized = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--validate")) validate = true;
        else if (!std::strcmp(argv[i], "--check")) check = true;
        else if (!std::strcmp(argv[i], "--no-vulkan-region")) vulkanRegion = false;
        else if (!std::strcmp(argv[i], "--all-features")) allFeatures = true;
        else if (!std::strcmp(argv[i], "--check-features")) checkFeatures = true;
        else if (!std::strcmp(argv[i], "--wake-check") && i + 1 < argc) wakes = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-wake")) sendWakes = false;
        else if (!std::strcmp(argv[i], "--minimized")) minimized = true;
        else {
            std::fprintf(stderr,
                         "usage: panelspun-demo [--frames N] [--screenshot out.bmp] [--check] [--validate] "
                         "[--no-vulkan-region] [--all-features] [--check-features] [--wake-check N [--no-wake] [--minimized]]\n");
            return 2;
        }
    }
    if (check && (screenshot.empty() || frames <= 0)) {
        std::fprintf(stderr, "--check needs --frames and --screenshot\n");
        return 2;
    }

    SplitTree tree("video");
    tree.dock("controls", "video", Side::Right, 0.32);
    tree.dock("actions", "controls", Side::Bottom, 0.5);
    tree.setMinSize("video", Size{240, 160});
    tree.setMinSize("controls", Size{200, 120});
    tree.setMinSize("actions", Size{200, 120});

    WindowConfig config;
    config.title = "panelspun demo";
    config.vulkanValidation = validate;
    config.vulkanAllFeatures = allFeatures;
    std::string error;
    std::unique_ptr<Window> window = Window::create(config, tree, &error);
    if (!window) {
        std::fprintf(stderr, "panelspun-demo: %s\n", error.c_str());
        return 1;
    }
    if (checkFeatures) {
        const VulkanContext& vk = window->vulkan();
        const bool chained = vk.deviceInfo && vk.deviceInfo->pNext;
        std::printf("features: api %u.%u, device features %s\n", VK_API_VERSION_MAJOR(vk.apiVersion),
                    VK_API_VERSION_MINOR(vk.apiVersion), chained ? "chained" : "none");
        if (vk.apiVersion < VK_API_VERSION_1_3 || !vk.instanceInfo || !chained) return 1;
    }

    std::atomic<int> draws{0};
    std::atomic<int> updates{0};
    window->setPanel("video", std::make_unique<VideoPanel>(vulkanRegion, &draws, &updates));

    std::unique_ptr<WidgetPanel> controls = std::make_unique<WidgetPanel>("Controls");
    Label* level = controls->add(std::make_unique<Label>("Exposure: 50%"));
    controls->add(std::make_unique<Slider>(0.0f, 100.0f, 50.0f, [level](float v) {
        level->setText("Exposure: " + std::to_string(static_cast<int>(v + 0.5f)) + "%");
    }));
    window->setPanel("controls", std::move(controls));

    std::unique_ptr<WidgetPanel> actions = std::make_unique<WidgetPanel>("Actions");
    Label* clicks = actions->add(std::make_unique<Label>("Clicks: 0"));
    std::shared_ptr<int> count = std::make_shared<int>(0);
    actions->add(std::make_unique<Button>("Click", [clicks, count]() { clicks->setText("Clicks: " + std::to_string(++*count)); }));
    Window* raw = window.get();
    actions->add(std::make_unique<Button>("Print layout", [raw]() { std::printf("%s", raw->layout().serialize().c_str()); }));
    window->setPanel("actions", std::move(actions));

    int rc = 0;
    if (wakes > 0) {
        // A worker thread asks for redraws; each must render at least once more than an idle window would.
        int before = 0;
        int updatesBefore = 0;
        if (minimized) {
            int count = 0;
            SDL_Window** windows = SDL_GetWindows(&count);
            for (int i = 0; i < count; ++i) {
                SDL_MinimizeWindow(windows[i]);
                SDL_SyncWindow(windows[i]);
            }
            SDL_free(windows);
        }
        std::thread worker([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            before = draws.load();
            updatesBefore = updates.load();
            for (int i = 0; i < wakes; ++i) {
                if (sendWakes) window->requestRedrawFromAnyThread();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        });
        rc = window->run();
        worker.join();
        const int rendered = draws.load() - before;
        const int updated = updates.load() - updatesBefore;
        std::printf("wake: %d redraws and %d updates for %d requests%s\n", rendered, updated, wakes,
                    minimized ? " while minimized" : "");
        if (minimized) {
            // A minimized window that still renders was never minimized, so the check would prove nothing.
            if (rc == 0 && rendered >= wakes) rc = 4;
            if (rc == 0 && updated < wakes) rc = 3;
        } else if (rc == 0 && rendered < wakes) {
            rc = 3;
        }
    } else if (frames > 0) {
        if (frames > 1) rc = window->run(frames - 1);
        if (!screenshot.empty()) window->captureNextFrame(screenshot);
        if (rc == 0) rc = window->run(1);
        if (!screenshot.empty() && !window->lastCaptureSucceeded()) {
            std::fprintf(stderr, "FAIL screenshot was not written\n");
            rc = 3;
        }
    } else {
        rc = window->run();
    }
    if (validate && window->validationErrors() > 0) {
        std::fprintf(stderr, "FAIL %d Vulkan validation errors\n", window->validationErrors());
        rc = 3;
    }
    SplitTree finalLayout = window->layout();
    window.reset();
    if (rc == 0 && check) rc = checkCapture(screenshot, finalLayout);
    SDL_Quit();
    return rc;
}
