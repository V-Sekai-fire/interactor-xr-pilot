// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "panelspun/panel.h"

namespace panelspun {

class Widget {
public:
    virtual ~Widget() = default;
    virtual float height(float scale) const = 0;
    virtual void draw(DrawContext& ctx, float x, float y, float w) = 0;
    virtual bool pointer(const PointerEvent& e, float x, float y, float w) {
        (void)e, (void)x, (void)y, (void)w;
        return false;
    }
};

class Label : public Widget {
public:
    explicit Label(std::string text, float size = 14.0f) : text_(std::move(text)), size_(size) {}
    void setText(std::string text) { text_ = std::move(text); }
    const std::string& text() const { return text_; }
    float height(float scale) const override;
    void draw(DrawContext& ctx, float x, float y, float w) override;

private:
    std::string text_;
    float size_;
};

class Button : public Widget {
public:
    Button(std::string text, std::function<void()> onClick) : text_(std::move(text)), onClick_(std::move(onClick)) {}
    float height(float scale) const override;
    void draw(DrawContext& ctx, float x, float y, float w) override;
    bool pointer(const PointerEvent& e, float x, float y, float w) override;

private:
    std::string text_;
    std::function<void()> onClick_;
    bool hover_ = false;
    bool pressed_ = false;
    float lastScale_ = 1.0f;
};

class Slider : public Widget {
public:
    Slider(float min, float max, float value, std::function<void(float)> onChange)
        : min_(min), max_(max), value_(value), onChange_(std::move(onChange)) {}
    float value() const { return value_; }
    float height(float scale) const override;
    void draw(DrawContext& ctx, float x, float y, float w) override;
    bool pointer(const PointerEvent& e, float x, float y, float w) override;

private:
    bool setFromX(float px, float x, float w);
    float min_;
    float max_;
    float value_;
    std::function<void(float)> onChange_;
    bool dragging_ = false;
    float lastScale_ = 1.0f;
};

// A panel that stacks widgets top to bottom with padding.
class WidgetPanel : public Panel {
public:
    explicit WidgetPanel(std::string title) : Panel(std::move(title)) {}
    template <typename T>
    T* add(std::unique_ptr<T> widget) {
        T* raw = widget.get();
        widgets_.push_back(std::move(widget));
        return raw;
    }
    void draw(DrawContext& ctx) override;
    bool pointer(const PointerEvent& e) override;

private:
    std::vector<std::unique_ptr<Widget>> widgets_;
    std::vector<float> tops_;
    float width_ = 0.0f;
    float scale_ = 1.0f;
    Widget* captured_ = nullptr;
};

}
