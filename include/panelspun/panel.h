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

enum class PointerAction : std::uint8_t { Down, Up, Move, Leave };

struct PointerEvent {
    PointerAction action = PointerAction::Move;
    float x = 0.0f;
    float y = 0.0f;
    int button = 0;
};

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

    // A panel returning true has its content rect filled by recordVulkan instead of draw.
    virtual bool usesVulkanRegion() const { return false; }
    virtual void recordVulkan(const VulkanRegionFrame& frame) { (void)frame; }

private:
    std::string title_;
};

}
