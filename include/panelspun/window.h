// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "panelspun/panel.h"
#include "panelspun/split_tree.h"
#include "panelspun/vulkan_region.h"

namespace panelspun {

struct WindowConfig {
    std::string title = "panelspun";
    int width = 1280;
    int height = 800;
    Theme theme;
    // Enables the Khronos validation layer; creation fails if the layer is not installed.
    bool vulkanValidation = false;
    // Creates a Vulkan 1.3 instance and device with every supported core feature enabled, for a
    // consumer recording compute work on the window's device; fails on a device below 1.3.
    bool vulkanAllFeatures = false;
    // Wakes the frame loop this many times a second even with no events, for held keys; 0 waits for events.
    int tickHz = 0;
    // Keeps the panels where the layout put them: split handles do not drag or highlight.
    bool lockLayout = false;
    // Width over height the window keeps while a person resizes it; 0 leaves it free.
    float aspectRatio = 0.0f;
    // Delivers each finger as its own pointer instead of letting SDL turn touches into one mouse.
    // A touch does not move keyboard focus.
    bool multiTouch = false;
    // Opens gamepads as they connect, for Window::gamepad; needs PANELSPUN_SDL_GAMEPAD at build time.
    bool gamepads = false;
    // Draws each panel's title bar; without them the content fills the whole leaf.
    bool panelHeaders = true;
};

// The first connected gamepad: sticks in -1..1 with +y down as SDL reports them, triggers in 0..1,
// and one bit per SDL_GamepadButton.
struct GamepadState {
    bool connected = false;
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    std::uint32_t buttons = 0;
    std::string name;
};

class Window {
public:
    // Returns nullptr and fills error when SDL, Vulkan or ThorVG cannot start.
    static std::unique_ptr<Window> create(const WindowConfig& config, SplitTree layout, std::string* error);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // The panel id must already be a leaf of the layout.
    bool setPanel(const std::string& id, std::unique_ptr<Panel> panel);

    SplitTree& layout();
    const VulkanContext& vulkan() const;

    // Pumps events and presents until the window closes or frameLimit frames are presented.
    int run(int frameLimit = 0);
    void requestRedraw();
    // Safe from any thread: wakes the frame loop and redraws, for content produced off the UI thread.
    void requestRedrawFromAnyThread();
    void requestClose();
    // Hides the cursor and reports motion as dx, dy to the panel that has the keyboard; focus loss turns it off.
    void setRelativeMouse(bool on);
    bool relativeMouse() const;
    // False when no gamepad is connected or gamepads are off.
    bool gamepad(GamepadState& state) const;

    // Copies the next presented frame to a BMP file.
    void captureNextFrame(std::string path);
    bool lastCaptureSucceeded() const;
    int validationErrors() const;

    struct Impl;

private:
    explicit Window(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}
