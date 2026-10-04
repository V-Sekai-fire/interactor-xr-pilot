// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

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
