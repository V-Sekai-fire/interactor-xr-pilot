// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include <string>

#include "panelspun/split_tree.h"
#include "panelspun/vulkan_region.h"

namespace tvg {
struct Scene;
}

namespace panelspun {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

struct Theme {
    Color background{30, 31, 34};
    Color panel{43, 45, 48};
    Color header{55, 57, 61};
    Color handle{22, 23, 25};
    Color handleActive{78, 132, 214};
    Color text{223, 225, 229};
    Color accent{78, 132, 214};
    Color control{70, 73, 78};
    Color controlHover{86, 90, 96};
};

// What a panel draws into: a scene in panel-local pixels, clipped to the panel.
struct DrawContext {
    tvg::Scene* scene = nullptr;
    int width = 0;
    int height = 0;
    float scale = 1.0f;
    const char* font = nullptr;
    const Theme* theme = nullptr;
};

// Wheel carries its notches in dy. While the mouse is relative, x and y stay put and dx, dy carry the motion.
enum class PointerAction : std::uint8_t { Down, Up, Move, Leave, Wheel };

struct PointerEvent {
    PointerAction action = PointerAction::Move;
    float x = 0.0f;
    float y = 0.0f;
    int button = 0;
    float dx = 0.0f;
    float dy = 0.0f;
};

// An SDL scancode, so keys keep their place whatever the layout; mod is SDL's SDL_Keymod.
struct KeyEvent {
    int scancode = 0;
    bool down = false;
    bool repeat = false;
    std::uint16_t mod = 0;
};

// A stroked open line through count points, in panel-local pixels.
void drawPolyline(DrawContext& ctx, const float* xy, int count, Color color, float width);

class Panel {
public:
    explicit Panel(std::string title) : title_(std::move(title)) {}
    virtual ~Panel() = default;

    const std::string& title() const { return title_; }

    virtual void draw(DrawContext& ctx) = 0;
    // Returns true when the event changed what the panel draws.
    virtual bool pointer(const PointerEvent& e) {
        (void)e;
        return false;
    }

    // Keys go to the panel last clicked. Returns true when the event changed what the panel draws.
    virtual bool key(const KeyEvent& e) {
        (void)e;
        return false;
    }
    // The window lost keyboard focus, so held keys and mouse capture should be let go.
    virtual void focusLost() {}

    // A panel returning true has its content rect filled by recordVulkan instead of draw.
    virtual bool usesVulkanRegion() const { return false; }
    virtual void recordVulkan(const VulkanRegionFrame& frame) { (void)frame; }
    // Runs on the window thread at every wake once the previous frame is done, minimized or not.
    virtual void update(const VulkanContext& context) { (void)context; }

private:
    std::string title_;
};

}
