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
    // The pointer holding the widget is gone without an Up, as on focus loss: let go of it.
    virtual void cancel() {}
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

// A labelled on/off box; setValue changes it without calling onChange.
class Toggle : public Widget {
public:
    Toggle(std::string text, bool value, std::function<void(bool)> onChange)
        : text_(std::move(text)), value_(value), onChange_(std::move(onChange)) {}
    bool value() const { return value_; }
    void setValue(bool value) { value_ = value; }
    float height(float scale) const override;
    void draw(DrawContext& ctx, float x, float y, float w) override;
    bool pointer(const PointerEvent& e, float x, float y, float w) override;

private:
    std::string text_;
    bool value_;
    std::function<void(bool)> onChange_;
    bool pressed_ = false;
    float lastScale_ = 1.0f;
};

// Pressed while a pointer holds it: onChange(true) on Down, onChange(false) on Up or cancel.
// setLit shows it pressed from elsewhere, such as a key or a gamepad.
class HoldButton : public Widget {
public:
    HoldButton(std::string text, std::function<void(bool)> onChange, float height = 32.0f)
        : text_(std::move(text)), onChange_(std::move(onChange)), height_(height) {}
    bool held() const { return owner_ != kNone; }
    void setLit(bool lit) { lit_ = lit; }
    float height(float scale) const override { return height_ * scale; }
    void draw(DrawContext& ctx, float x, float y, float w) override;
    bool pointer(const PointerEvent& e, float x, float y, float w) override;
    void cancel() override;

private:
    static constexpr std::uint64_t kNone = ~std::uint64_t(0);
    std::string text_;
    std::function<void(bool)> onChange_;
    float height_;
    std::uint64_t owner_ = kNone;
    bool lit_ = false;
    float lastScale_ = 1.0f;
};

// A round pad as wide as it is tall: dragging reports x, y in -1..1 (y up) clamped to the circle,
// and letting go recentres it. setShown places the knob for a value set elsewhere.
class TouchStick : public Widget {
public:
    explicit TouchStick(std::function<void(float, float)> onChange) : onChange_(std::move(onChange)) {}
    float x() const { return x_; }
    float y() const { return y_; }
    bool held() const { return owner_ != kNone; }
    void setShown(float x, float y) { shownX_ = x, shownY_ = y; }
    void setLit(bool lit) { lit_ = lit; }
    float height(float scale) const override { (void)scale; return lastWidth_; }
    void draw(DrawContext& ctx, float x, float y, float w) override;
    bool pointer(const PointerEvent& e, float x, float y, float w) override;
    void cancel() override;

private:
    static constexpr std::uint64_t kNone = ~std::uint64_t(0);
    bool setFrom(const PointerEvent& e, float x, float y, float w);
    std::function<void(float, float)> onChange_;
    std::uint64_t owner_ = kNone;
    float x_ = 0.0f;
    float y_ = 0.0f;
    float shownX_ = 0.0f;
    float shownY_ = 0.0f;
    bool lit_ = false;
    float lastWidth_ = 96.0f;
};

// A labelled vertical bar for an analog value in 0..1: the touch height sets it, from 0 at the bottom
// to 1 at the top, and letting go returns it to 0. setShown fills it for a value set elsewhere.
class TouchBar : public Widget {
public:
    TouchBar(std::string text, std::function<void(float)> onChange, float height = 96.0f)
        : text_(std::move(text)), onChange_(std::move(onChange)), height_(height) {}
    float value() const { return value_; }
    void setShown(float value) { shown_ = value; }
    float height(float scale) const override { return height_ * scale; }
    void draw(DrawContext& ctx, float x, float y, float w) override;
    bool pointer(const PointerEvent& e, float x, float y, float w) override;
    void cancel() override;

private:
    static constexpr std::uint64_t kNone = ~std::uint64_t(0);
    bool setFrom(float py, float y);
    std::string text_;
    std::function<void(float)> onChange_;
    float height_;
    std::uint64_t owner_ = kNone;
    float value_ = 0.0f;
    float shown_ = 0.0f;
    float lastScale_ = 1.0f;
};

// Where a widget sits in its panel, in panel-local pixels; its height is the widget's own.
struct Placement {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
};

// A panel of widgets, stacked top to bottom with padding unless a subclass arranges them. Each pointer
// (the mouse, or each finger) is held by the widget it went down on until it comes up.
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
    void focusLost() override;

protected:
    // One placement per widget, in the order they were added, for a panel width by height pixels.
    virtual std::vector<Placement> arrange(float width, float height, float scale);
    const std::vector<std::unique_ptr<Widget>>& widgets() const { return widgets_; }

private:
    std::vector<std::unique_ptr<Widget>> widgets_;
    std::vector<Placement> placements_;
    float scale_ = 1.0f;
    std::vector<std::pair<std::uint64_t, Widget*>> captured_;
};

}
