// SPDX-License-Identifier: Apache-2.0 OR MIT
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

constexpr std::uint8_t kLineRgb[3] = {230, 40, 200};

// Counts the input that reaches it; with a line, draws a thick one across its middle.
class ProbePanel : public Panel {
public:
    ProbePanel(Window* window, bool line) : Panel("Probe"), window_(window), line_(line) {}
    void draw(DrawContext& ctx) override {
        if (!line_) return;
        float y = ctx.height * 0.5f;
        float xy[6] = {0.0f, y, ctx.width * 0.5f, y, static_cast<float>(ctx.width), y};
        drawPolyline(ctx, xy, 3, Color{kLineRgb[0], kLineRgb[1], kLineRgb[2], 255}, 12.0f);
    }
    bool pointer(const PointerEvent& e) override {
        if (e.action == PointerAction::Down) window_->setRelativeMouse(true);
        if (e.action == PointerAction::Wheel) wheel += e.dy;
        if (e.action == PointerAction::Move) {
            dx += e.dx;
            dy += e.dy;
        }
        return false;
    }
    bool key(const KeyEvent& e) override {
        (void)e;
        ++keys;
        return false;
    }
    void focusLost() override { ++focusLosses; }
    void update(const VulkanContext& context) override {
        (void)context;
        ++updates;
    }

    int keys = 0;
    int focusLosses = 0;
    std::atomic<int> updates{0};
    float wheel = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;

private:
    Window* window_;
    bool line_;
};

// Two hold buttons side by side over a touch stick, recording what each finger did to them.
class TouchProbe : public WidgetPanel {
public:
    TouchProbe() : WidgetPanel("Touch") {
        add(std::make_unique<HoldButton>("A", [this](bool on) { aHeld = on; }, 64.0f));
        add(std::make_unique<HoldButton>("B", [this](bool on) { bHeld = on; }, 64.0f));
        add(std::make_unique<TouchStick>([this](float x, float y) {
            stickX = x;
            (void)y;
        }));
    }
    std::vector<Placement> placements() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_;
    }
    std::atomic<bool> aHeld{false};
    std::atomic<bool> bHeld{false};
    std::atomic<float> stickX{0.0f};

protected:
    std::vector<Placement> arrange(float width, float height, float scale) override {
        (void)height;
        float pad = 12.0f * scale;
        float half = (width - 3.0f * pad) * 0.5f;
        float stick = 160.0f * scale;
        std::vector<Placement> out = {Placement{pad, pad, half}, Placement{2.0f * pad + half, pad, half},
                                      Placement{pad, pad + 64.0f * scale + pad, stick}};
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = out;
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<Placement> last_;
};

struct FingerTarget {
    SDL_WindowID window = 0;
    float width = 1.0f;
    float height = 1.0f;
    float header = 0.0f;
};

void post(SDL_Event e);

// A finger event at a point in the window's first panel, below its header, in pixels.
void postFinger(const FingerTarget& t, Uint32 type, SDL_FingerID finger, float x, float y) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.touchID = 1;
    e.tfinger.fingerID = finger;
    e.tfinger.windowID = t.window;
    e.tfinger.x = x / t.width;
    e.tfinger.y = (y + t.header) / t.height;
    e.tfinger.pressure = 1.0f;
    post(e);
}

void post(SDL_Event e) {
    SDL_PushEvent(&e);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

bool pixelMatches(SDL_Surface* s, int x, int y, const std::uint8_t rgb[3]) {
    Uint8 r = 0, g = 0, b = 0, a = 0;
    if (!SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a)) return false;
    return std::abs(r - rgb[0]) <= 1 && std::abs(g - rgb[1]) <= 1 && std::abs(b - rgb[2]) <= 1;
}

// The video region must show the Vulkan clear colour and the controls panel must not.
int checkCapture(const std::string& path, SplitTree tree, bool line) {
    SDL_Surface* s = SDL_LoadBMP(path.c_str());
    if (!s) {
        std::fprintf(stderr, "FAIL could not read %s: %s\n", path.c_str(), SDL_GetError());
        return 3;
    }
    std::vector<LeafRect> leaves = tree.layout(Rect{0, 0, s->w, s->h});
    int failures = 0;
    if (line) {
        for (const LeafRect& l : leaves) {
            if (l.id != "video") continue;
            int x = l.rect.x + l.rect.w / 2;
            int y = l.rect.y + 12 + (l.rect.h - 12) / 2;
            bool hit = false;
            for (int dy = -40; dy <= 40 && !hit; ++dy) hit = pixelMatches(s, x, y + dy, kLineRgb);
            std::printf("%s the polyline colour %s near (%d,%d)\n", hit ? "PASS" : "FAIL", hit ? "is" : "is not", x, y);
            failures += hit ? 0 : 1;
        }
        SDL_DestroySurface(s);
        return failures == 0 ? 0 : 3;
    }
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
    bool inputCheck = false;
    bool focusClick = true;
    int tickCheck = 0;
    bool tick = true;
    bool polylineCheck = false;
    bool polyline = true;
    bool lockCheck = false;
    bool locked = true;
    bool touchCheck = false;
    bool multiTouch = true;
    bool gamepadCheck = false;
    bool gamepads = true;
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
        else if (!std::strcmp(argv[i], "--input-check")) inputCheck = true;
        else if (!std::strcmp(argv[i], "--no-focus-click")) focusClick = false;
        else if (!std::strcmp(argv[i], "--tick-check") && i + 1 < argc) tickCheck = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-tick")) tick = false;
        else if (!std::strcmp(argv[i], "--polyline-check")) polylineCheck = true;
        else if (!std::strcmp(argv[i], "--no-polyline")) polyline = false;
        else if (!std::strcmp(argv[i], "--lock-check")) lockCheck = true;
        else if (!std::strcmp(argv[i], "--unlocked")) locked = false;
        else if (!std::strcmp(argv[i], "--touch-check")) touchCheck = true;
        else if (!std::strcmp(argv[i], "--single-touch")) multiTouch = false;
        else if (!std::strcmp(argv[i], "--gamepad-check")) gamepadCheck = true;
        else if (!std::strcmp(argv[i], "--no-gamepads")) gamepads = false;
        else {
            std::fprintf(stderr,
                         "usage: panelspun-demo [--frames N] [--screenshot out.bmp] [--check] [--validate] "
                         "[--no-vulkan-region] [--all-features] [--check-features] [--wake-check N [--no-wake] [--minimized]] "
                         "[--input-check [--no-focus-click]] [--tick-check HZ [--no-tick]] [--polyline-check [--no-polyline]] "
                         "[--lock-check [--unlocked]] [--touch-check [--single-touch]] [--gamepad-check [--no-gamepads]]\n");
            return 2;
        }
    }
    if (check && (screenshot.empty() || frames <= 0)) {
        std::fprintf(stderr, "--check needs --frames and --screenshot\n");
        return 2;
    }

    SplitTree tree("video");
    if (!touchCheck) {
        tree.dock("controls", "video", Side::Right, 0.32);
        tree.dock("actions", "controls", Side::Bottom, 0.5);
        tree.setMinSize("controls", Size{200, 120});
        tree.setMinSize("actions", Size{200, 120});
    }
    tree.setMinSize("video", Size{240, 160});
    // The window's gamepad support is what the check is about, so SDL itself always has gamepads.
    if (gamepadCheck) SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    WindowConfig config;
    config.title = "panelspun demo";
    config.vulkanValidation = validate;
    config.vulkanAllFeatures = allFeatures;
    config.tickHz = tickCheck > 0 && tick ? tickCheck : 0;
    config.lockLayout = lockCheck && locked;
    config.multiTouch = touchCheck && multiTouch;
    config.gamepads = gamepadCheck && gamepads;
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
    ProbePanel* probe = nullptr;
    TouchProbe* touch = nullptr;
    if (touchCheck) {
        std::unique_ptr<TouchProbe> t = std::make_unique<TouchProbe>();
        touch = t.get();
        window->setPanel("video", std::move(t));
    } else if (inputCheck || tickCheck > 0 || polylineCheck) {
        std::unique_ptr<ProbePanel> p = std::make_unique<ProbePanel>(window.get(), polylineCheck && polyline);
        probe = p.get();
        window->setPanel("video", std::move(p));
    } else {
        window->setPanel("video", std::make_unique<VideoPanel>(vulkanRegion, &draws, &updates));
    }

    std::unique_ptr<WidgetPanel> controls = std::make_unique<WidgetPanel>("Controls");
    Label* level = controls->add(std::make_unique<Label>("Exposure: 50%"));
    controls->add(std::make_unique<Slider>(0.0f, 100.0f, 50.0f, [level](float v) {
        level->setText("Exposure: " + std::to_string(static_cast<int>(v + 0.5f)) + "%");
    }));
    if (!touchCheck) window->setPanel("controls", std::move(controls));

    std::unique_ptr<WidgetPanel> actions = std::make_unique<WidgetPanel>("Actions");
    Label* clicks = actions->add(std::make_unique<Label>("Clicks: 0"));
    std::shared_ptr<int> count = std::make_shared<int>(0);
    actions->add(std::make_unique<Button>("Click", [clicks, count]() { clicks->setText("Clicks: " + std::to_string(++*count)); }));
    Window* raw = window.get();
    actions->add(std::make_unique<Button>("Print layout", [raw]() { std::printf("%s", raw->layout().serialize().c_str()); }));
    window->setPanel("actions", std::move(actions));

    int rc = 0;
    if (touchCheck) {
        // Two fingers hold A and B together; a third drags the stick to its right edge and lets go.
        bool bothHeld = false;
        bool releasedAll = false;
        float stickAtEdge = 0.0f;
        float stickAfter = 1.0f;
        std::thread worker([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            int count = 0;
            SDL_Window** windows = SDL_GetWindows(&count);
            SDL_Window* w = count > 0 ? windows[0] : nullptr;
            SDL_free(windows);
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(w, &pw, &ph);
            const SDL_WindowID id = SDL_GetWindowID(w);
            const float header = std::round(24.0f * SDL_GetWindowDisplayScale(w));
            std::vector<Placement> p = touch->placements();
            for (int wait = 0; wait < 50 && p.size() < 3; ++wait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                p = touch->placements();
            }
            if (p.size() < 3) {
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
                return;
            }
            const FingerTarget target{id, static_cast<float>(pw), static_cast<float>(ph), header};
            const float buttonY = p[0].y + 32.0f;
            postFinger(target, SDL_EVENT_FINGER_DOWN, 10, p[0].x + p[0].w * 0.5f, buttonY);
            postFinger(target, SDL_EVENT_FINGER_DOWN, 11, p[1].x + p[1].w * 0.5f, buttonY);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            bothHeld = touch->aHeld && touch->bHeld;
            const float r = p[2].w * 0.5f;
            postFinger(target, SDL_EVENT_FINGER_DOWN, 12, p[2].x + r, p[2].y + r);
            postFinger(target, SDL_EVENT_FINGER_MOTION, 12, p[2].x + 2.0f * r, p[2].y + r);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stickAtEdge = touch->stickX;
            postFinger(target, SDL_EVENT_FINGER_UP, 12, p[2].x + 2.0f * r, p[2].y + r);
            postFinger(target, SDL_EVENT_FINGER_UP, 10, p[0].x + p[0].w * 0.5f, buttonY);
            postFinger(target, SDL_EVENT_FINGER_UP, 11, p[1].x + p[1].w * 0.5f, buttonY);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stickAfter = touch->stickX;
            releasedAll = !touch->aHeld && !touch->bHeld;
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        });
        rc = window->run();
        worker.join();
        std::printf("touch: A and B held together %s, stick at edge %.2f, after release %.2f, all released %s%s\n",
                    bothHeld ? "yes" : "no", stickAtEdge, stickAfter, releasedAll ? "yes" : "no",
                    multiTouch ? "" : " (single touch)");
        const bool ok = bothHeld && stickAtEdge > 0.95f && std::abs(stickAfter) < 0.01f && releasedAll;
        if (rc == 0 && !ok) rc = 3;
    } else if (gamepadCheck) {
        // A virtual pad presses South and pushes its left stick right; the window must report both.
        SDL_JoystickID padId = 0;
        SDL_Joystick* joystick = nullptr;
        std::thread worker([&]() {
            SDL_VirtualJoystickDesc desc;
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
            desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
            desc.name = "panelspun virtual pad";
            padId = SDL_AttachVirtualJoystick(&desc);
            joystick = padId ? SDL_OpenJoystick(padId) : nullptr;
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (joystick) {
                SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
                SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        });
        rc = window->run();
        worker.join();
        GamepadState pad;
        const bool reported = window->gamepad(pad);
        std::printf("gamepad: %s '%s', south %s, left x %.2f%s\n", reported ? "reported" : "not reported",
                    pad.name.c_str(), (pad.buttons & (1u << SDL_GAMEPAD_BUTTON_SOUTH)) ? "down" : "up", pad.leftX,
                    gamepads ? "" : " (gamepads off)");
        if (joystick) SDL_CloseJoystick(joystick);
        if (padId) SDL_DetachVirtualJoystick(padId);
        const bool ok = reported && (pad.buttons & (1u << SDL_GAMEPAD_BUTTON_SOUTH)) && pad.leftX > 0.95f;
        if (rc == 0 && !ok) rc = 3;
    } else if (lockCheck) {
        // A worker drags the first split handle 100 px; a locked layout must not move.
        std::string before;
        std::thread worker([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            before = window->layout().serialize();
            int count = 0;
            SDL_Window** windows = SDL_GetWindows(&count);
            SDL_Window* w = count > 0 ? windows[0] : nullptr;
            SDL_free(windows);
            const SDL_WindowID id = SDL_GetWindowID(w);
            const float density = std::max(1.0f, SDL_GetWindowPixelDensity(w));
            const SplitHandle h = window->layout().handles().front();
            const float x = (h.rect.x + h.rect.w * 0.5f) / density;
            const float y = (h.rect.y + h.rect.h * 0.5f) / density;
            SDL_Event down{};
            down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            down.button.windowID = id;
            down.button.button = SDL_BUTTON_LEFT;
            down.button.down = true;
            down.button.clicks = 1;
            down.button.x = x;
            down.button.y = y;
            post(down);
            SDL_Event move{};
            move.type = SDL_EVENT_MOUSE_MOTION;
            move.motion.windowID = id;
            move.motion.state = SDL_BUTTON_LMASK;
            move.motion.x = h.axis == Axis::Horizontal ? x - 100.0f : x;
            move.motion.y = h.axis == Axis::Horizontal ? y : y - 100.0f;
            post(move);
            SDL_Event up = down;
            up.type = SDL_EVENT_MOUSE_BUTTON_UP;
            up.button.down = false;
            up.button.x = move.motion.x;
            up.button.y = move.motion.y;
            post(up);
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        });
        rc = window->run();
        worker.join();
        const bool moved = window->layout().serialize() != before;
        std::printf("lock: the layout %s after a 100 px handle drag%s\n", moved ? "moved" : "held",
                    locked ? "" : " (unlocked)");
        if (rc == 0 && moved) rc = 3;
    } else if (inputCheck) {
        // A worker posts a click, a key, a wheel notch, relative motion and a focus loss, as SDL would.
        std::thread worker([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            int count = 0;
            SDL_Window** windows = SDL_GetWindows(&count);
            SDL_Window* w = count > 0 ? windows[0] : nullptr;
            SDL_free(windows);
            int ww = 0, wh = 0;
            SDL_GetWindowSize(w, &ww, &wh);
            const SDL_WindowID id = SDL_GetWindowID(w);
            const float cx = ww * 0.3f, cy = wh * 0.5f;
            if (focusClick) {
                for (bool down : {true, false}) {
                    SDL_Event e{};
                    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                    e.button.windowID = id;
                    e.button.button = SDL_BUTTON_LEFT;
                    e.button.down = down;
                    e.button.clicks = 1;
                    e.button.x = cx;
                    e.button.y = cy;
                    post(e);
                }
            }
            for (bool down : {true, false}) {
                SDL_Event e{};
                e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                e.key.windowID = id;
                e.key.scancode = SDL_SCANCODE_W;
                e.key.key = SDLK_W;
                e.key.down = down;
                post(e);
            }
            SDL_Event wheel{};
            wheel.type = SDL_EVENT_MOUSE_WHEEL;
            wheel.wheel.windowID = id;
            wheel.wheel.y = 2.0f;
            wheel.wheel.mouse_x = cx;
            wheel.wheel.mouse_y = cy;
            post(wheel);
            SDL_Event motion{};
            motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.windowID = id;
            motion.motion.x = cx;
            motion.motion.y = cy;
            motion.motion.xrel = 10.0f;
            motion.motion.yrel = -4.0f;
            post(motion);
            SDL_Event lost{};
            lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
            lost.window.windowID = id;
            post(lost);
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        });
        rc = window->run();
        worker.join();
        const bool relativeAfter = window->relativeMouse();
        std::printf("input: %d keys, wheel %.1f, motion (%.1f, %.1f), %d focus losses, relative after loss %s\n",
                    probe->keys, probe->wheel, probe->dx, probe->dy, probe->focusLosses, relativeAfter ? "on" : "off");
        const bool ok = probe->keys == 2 && probe->wheel == 2.0f && std::abs(probe->dx - 10.0f) < 0.5f &&
                        std::abs(probe->dy + 4.0f) < 0.5f && probe->focusLosses >= 1 && !relativeAfter;
        if (rc == 0 && !ok) rc = 3;
    } else if (tickCheck > 0) {
        // No events at all for a second: only the tick can wake the loop.
        std::thread worker([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1400));
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        });
        int before = 0;
        std::thread sampler([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            before = probe->updates.load();
        });
        rc = window->run();
        worker.join();
        sampler.join();
        const int ticked = probe->updates.load() - before;
        std::printf("tick: %d updates in about 1 s at %d Hz requested%s\n", ticked, tickCheck, tick ? "" : " (tick off)");
        if (rc == 0 && ticked < tickCheck / 2) rc = 3;
    } else if (wakes > 0) {
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
    if (rc == 0 && check) rc = checkCapture(screenshot, finalLayout, polylineCheck);
    SDL_Quit();
    return rc;
}
