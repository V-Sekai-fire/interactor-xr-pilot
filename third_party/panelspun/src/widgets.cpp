// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "panelspun/widgets.h"

#include <thorvg.h>

#include <algorithm>

namespace panelspun {

namespace {

// ThorVG text sizes are points at 96 dpi; widget sizes are pixels at scale 1.
constexpr float kPointsPerPixel = 0.75f;

void addText(DrawContext& ctx, const std::string& s, float size, float x, float y, float ax, Color c) {
    tvg::Text* t = tvg::Text::gen();
    t->font(ctx.font);
    t->size(size * ctx.scale * kPointsPerPixel);
    t->text(s.c_str());
    t->fill(c.r, c.g, c.b);
    t->align(ax, 0.5f);
    t->translate(x, y);
    ctx.scene->add(t);
}

void addRect(DrawContext& ctx, float x, float y, float w, float h, float radius, Color c) {
    tvg::Shape* s = tvg::Shape::gen();
    s->appendRect(x, y, w, h, radius, radius);
    s->fill(c.r, c.g, c.b, c.a);
    ctx.scene->add(s);
}

bool inside(const PointerEvent& e, float x, float y, float w, float h) {
    return e.x >= x && e.y >= y && e.x < x + w && e.y < y + h;
}

}

float Label::height(float scale) const { return size_ * 1.6f * scale; }

void Label::draw(DrawContext& ctx, float x, float y, float w) {
    (void)w;
    addText(ctx, text_, size_, x, y + height(ctx.scale) * 0.5f, 0.0f, ctx.theme->text);
}

float Button::height(float scale) const { return 32.0f * scale; }

void Button::draw(DrawContext& ctx, float x, float y, float w) {
    lastScale_ = ctx.scale;
    float h = height(ctx.scale);
    Color fill = pressed_ ? ctx.theme->accent : hover_ ? ctx.theme->controlHover : ctx.theme->control;
    addRect(ctx, x, y, w, h, 4.0f * ctx.scale, fill);
    addText(ctx, text_, 13.0f, x + w * 0.5f, y + h * 0.5f, 0.5f, ctx.theme->text);
}

bool Button::pointer(const PointerEvent& e, float x, float y, float w) {
    bool in = inside(e, x, y, w, height(lastScale_));
    bool wasHover = hover_;
    bool wasPressed = pressed_;
    switch (e.action) {
    case PointerAction::Move:
        hover_ = in;
        break;
    case PointerAction::Down:
        pressed_ = in && e.button == 1;
        break;
    case PointerAction::Up:
        if (pressed_ && in && onClick_) onClick_();
        pressed_ = false;
        hover_ = in;
        return true;
    case PointerAction::Leave:
        hover_ = false;
        break;
    }
    return hover_ != wasHover || pressed_ != wasPressed;
}

float Slider::height(float scale) const { return 28.0f * scale; }

void Slider::draw(DrawContext& ctx, float x, float y, float w) {
    lastScale_ = ctx.scale;
    float h = height(ctx.scale);
    float knob = 8.0f * ctx.scale;
    float trackX = x + knob;
    float trackW = std::max(0.0f, w - 2.0f * knob);
    float t = max_ > min_ ? (value_ - min_) / (max_ - min_) : 0.0f;
    float cy = y + h * 0.5f;
    addRect(ctx, trackX, cy - 2.0f * ctx.scale, trackW, 4.0f * ctx.scale, 2.0f * ctx.scale, ctx.theme->control);
    addRect(ctx, trackX, cy - 2.0f * ctx.scale, trackW * t, 4.0f * ctx.scale, 2.0f * ctx.scale, ctx.theme->accent);
    tvg::Shape* k = tvg::Shape::gen();
    k->appendCircle(trackX + trackW * t, cy, knob, knob);
    Color c = dragging_ ? ctx.theme->accent : ctx.theme->text;
    k->fill(c.r, c.g, c.b, c.a);
    ctx.scene->add(k);
}

bool Slider::setFromX(float px, float x, float w) {
    float knob = 8.0f * lastScale_;
    float trackW = std::max(1.0f, w - 2.0f * knob);
    float t = std::clamp((px - x - knob) / trackW, 0.0f, 1.0f);
    float v = min_ + t * (max_ - min_);
    if (v == value_) return false;
    value_ = v;
    if (onChange_) onChange_(value_);
    return true;
}

bool Slider::pointer(const PointerEvent& e, float x, float y, float w) {
    switch (e.action) {
    case PointerAction::Down:
        if (e.button != 1 || !inside(e, x, y, w, height(lastScale_))) return false;
        dragging_ = true;
        setFromX(e.x, x, w);
        return true;
    case PointerAction::Move:
        return dragging_ && setFromX(e.x, x, w);
    case PointerAction::Up:
        if (!dragging_) return false;
        dragging_ = false;
        return true;
    case PointerAction::Leave:
        return false;
    }
    return false;
}

void WidgetPanel::draw(DrawContext& ctx) {
    scale_ = ctx.scale;
    float pad = 12.0f * ctx.scale;
    width_ = std::max(0.0f, ctx.width - 2.0f * pad);
    tops_.clear();
    float y = pad;
    for (const std::unique_ptr<Widget>& w : widgets_) {
        tops_.push_back(y);
        w->draw(ctx, pad, y, width_);
        y += w->height(ctx.scale) + 8.0f * ctx.scale;
    }
}

bool WidgetPanel::pointer(const PointerEvent& e) {
    if (tops_.size() != widgets_.size()) return false;
    float pad = 12.0f * scale_;
    bool changed = false;
    for (std::size_t i = 0; i < widgets_.size(); ++i) {
        Widget* w = widgets_[i].get();
        if (captured_ && w != captured_ && e.action != PointerAction::Leave) continue;
        if (e.action == PointerAction::Down) {
            bool hit = e.x >= pad && e.x < pad + width_ && e.y >= tops_[i] && e.y < tops_[i] + w->height(scale_);
            if (!hit) continue;
            captured_ = w;
        }
        changed = w->pointer(e, pad, tops_[i], width_) || changed;
    }
    if (e.action == PointerAction::Up) captured_ = nullptr;
    return changed;
}

}
