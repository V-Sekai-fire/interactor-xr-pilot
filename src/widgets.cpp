// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "panelspun/widgets.h"

#include <thorvg.h>

#include <algorithm>
#include <cmath>

namespace panelspun {

namespace {

// ThorVG text sizes are points at 96 dpi; widget sizes are pixels at scale 1.
constexpr float kPointsPerPixel = 0.75f;

void addText(DrawContext& ctx, const std::string& s, float size, float x, float y, float ax, Color c) {
    drawText(ctx, s, size, x, y, ax, c);
}

void addRect(DrawContext& ctx, float x, float y, float w, float h, float radius, Color c) {
    drawRect(ctx, x, y, w, h, radius, c);
}

Color mix(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color{static_cast<std::uint8_t>(a.r + (b.r - a.r) * t), static_cast<std::uint8_t>(a.g + (b.g - a.g) * t),
                 static_cast<std::uint8_t>(a.b + (b.b - a.b) * t), static_cast<std::uint8_t>(a.a + (b.a - a.a) * t)};
}

bool inside(const PointerEvent& e, float x, float y, float w, float h) {
    return e.x >= x && e.y >= y && e.x < x + w && e.y < y + h;
}

}

void drawRect(DrawContext& ctx, float x, float y, float w, float h, float radius, Color color) {
    tvg::Shape* s = tvg::Shape::gen();
    s->appendRect(x, y, w, h, radius, radius);
    s->fill(color.r, color.g, color.b, color.a);
    ctx.scene->add(s);
}

void drawCircle(DrawContext& ctx, float cx, float cy, float r, Color color, float strokeWidth) {
    tvg::Shape* s = tvg::Shape::gen();
    s->appendCircle(cx, cy, r, r);
    if (strokeWidth > 0.0f) {
        s->strokeWidth(strokeWidth);
        s->strokeFill(color.r, color.g, color.b, color.a);
    } else {
        s->fill(color.r, color.g, color.b, color.a);
    }
    ctx.scene->add(s);
}

void drawText(DrawContext& ctx, const std::string& text, float size, float x, float y, float alignX, Color color) {
    tvg::Text* t = tvg::Text::gen();
    t->font(ctx.font);
    t->size(size * ctx.scale * kPointsPerPixel);
    t->text(text.c_str());
    t->fill(color.r, color.g, color.b);
    t->align(alignX, 0.5f);
    t->translate(x, y);
    ctx.scene->add(t);
}

void drawPolyline(DrawContext& ctx, const float* xy, int count, Color color, float width) {
    if (count < 2) return;
    tvg::Shape* s = tvg::Shape::gen();
    s->moveTo(xy[0], xy[1]);
    for (int i = 1; i < count; ++i) s->lineTo(xy[2 * i], xy[2 * i + 1]);
    s->strokeWidth(width);
    s->strokeFill(color.r, color.g, color.b, color.a);
    s->strokeJoin(tvg::StrokeJoin::Round);
    s->strokeCap(tvg::StrokeCap::Round);
    ctx.scene->add(s);
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
    case PointerAction::Wheel:
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
    case PointerAction::Wheel:
        return false;
    }
    return false;
}

float Toggle::height(float scale) const { return 28.0f * scale; }

void Toggle::draw(DrawContext& ctx, float x, float y, float w) {
    (void)w;
    lastScale_ = ctx.scale;
    float h = height(ctx.scale);
    float box = 16.0f * ctx.scale;
    float by = y + (h - box) * 0.5f;
    addRect(ctx, x, by, box, box, 3.0f * ctx.scale, value_ ? ctx.theme->accent : ctx.theme->control);
    if (value_) addRect(ctx, x + box * 0.3f, by + box * 0.3f, box * 0.4f, box * 0.4f, 1.0f * ctx.scale, ctx.theme->text);
    addText(ctx, text_, 13.0f, x + box + 8.0f * ctx.scale, y + h * 0.5f, 0.0f, ctx.theme->text);
}

bool Toggle::pointer(const PointerEvent& e, float x, float y, float w) {
    bool in = inside(e, x, y, w, height(lastScale_));
    switch (e.action) {
    case PointerAction::Down:
        pressed_ = in && e.button == 1;
        return false;
    case PointerAction::Up:
        if (!pressed_) return false;
        pressed_ = false;
        if (!in) return false;
        value_ = !value_;
        if (onChange_) onChange_(value_);
        return true;
    case PointerAction::Move:
    case PointerAction::Leave:
    case PointerAction::Wheel:
        return false;
    }
    return false;
}

void HoldButton::draw(DrawContext& ctx, float x, float y, float w) {
    lastScale_ = ctx.scale;
    float h = height(ctx.scale);
    addRect(ctx, x, y, w, h, 6.0f * ctx.scale, held() || lit_ ? ctx.theme->accent : ctx.theme->control);
    addText(ctx, text_, 13.0f, x + w * 0.5f, y + h * 0.5f, 0.5f, ctx.theme->text);
}

bool HoldButton::pointer(const PointerEvent& e, float x, float y, float w) {
    switch (e.action) {
    case PointerAction::Down:
        if (held() || e.button != 1 || !inside(e, x, y, w, height(lastScale_))) return false;
        owner_ = e.pointerId;
        if (onChange_) onChange_(true);
        return true;
    case PointerAction::Up:
        if (owner_ != e.pointerId) return false;
        cancel();
        return true;
    case PointerAction::Move:
    case PointerAction::Leave:
    case PointerAction::Wheel:
        return false;
    }
    return false;
}

void HoldButton::cancel() {
    if (!held()) return;
    owner_ = kNone;
    if (onChange_) onChange_(false);
}

void TouchStick::draw(DrawContext& ctx, float x, float y, float w) {
    lastWidth_ = w;
    float r = w * 0.5f;
    float cx = x + r;
    float cy = y + r;
    drawCircle(ctx, cx, cy, r, ctx.theme->control);
    drawCircle(ctx, cx, cy, r * 0.5f, ctx.theme->controlHover, 1.5f * ctx.scale);
    float kx = held() ? x_ : shownX_;
    float ky = held() ? y_ : shownY_;
    bool active = held() || lit_ || kx != 0.0f || ky != 0.0f;
    float knob = r * 0.38f;
    float travel = r - knob;
    drawCircle(ctx, cx + kx * travel, cy - ky * travel, knob, active ? ctx.theme->accent : ctx.theme->text);
}

bool TouchStick::setFrom(const PointerEvent& e, float x, float y, float w) {
    float r = std::max(1.0f, w * 0.5f);
    float knob = r * 0.38f;
    float travel = std::max(1.0f, r - knob);
    float nx = (e.x - (x + r)) / travel;
    float ny = -(e.y - (y + r)) / travel;
    float length = std::sqrt(nx * nx + ny * ny);
    if (length > 1.0f) {
        nx /= length;
        ny /= length;
    }
    if (nx == x_ && ny == y_) return false;
    x_ = nx;
    y_ = ny;
    if (onChange_) onChange_(x_, y_);
    return true;
}

bool TouchStick::pointer(const PointerEvent& e, float x, float y, float w) {
    switch (e.action) {
    case PointerAction::Down: {
        float r = w * 0.5f;
        float dx = e.x - (x + r);
        float dy = e.y - (y + r);
        if (held() || e.button != 1 || dx * dx + dy * dy > r * r) return false;
        owner_ = e.pointerId;
        setFrom(e, x, y, w);
        return true;
    }
    case PointerAction::Move:
        return owner_ == e.pointerId && setFrom(e, x, y, w);
    case PointerAction::Up:
        if (owner_ != e.pointerId) return false;
        cancel();
        return true;
    case PointerAction::Leave:
    case PointerAction::Wheel:
        return false;
    }
    return false;
}

void TouchStick::cancel() {
    if (!held()) return;
    owner_ = kNone;
    x_ = 0.0f;
    y_ = 0.0f;
    if (onChange_) onChange_(0.0f, 0.0f);
}

void TouchBar::draw(DrawContext& ctx, float x, float y, float w) {
    lastScale_ = ctx.scale;
    float h = height(ctx.scale);
    float label = 18.0f * ctx.scale;
    float barH = std::max(0.0f, h - label);
    float v = owner_ != kNone ? value_ : shown_;
    addRect(ctx, x, y, w, barH, 6.0f * ctx.scale, ctx.theme->control);
    if (v > 0.0f)
        addRect(ctx, x, y + barH * (1.0f - v), w, barH * v, 6.0f * ctx.scale,
                mix(ctx.theme->controlHover, ctx.theme->accent, 0.35f + 0.65f * v));
    addText(ctx, text_, 12.0f, x + w * 0.5f, y + barH + label * 0.5f, 0.5f, ctx.theme->text);
}

bool TouchBar::setFrom(float py, float y) {
    float barH = std::max(1.0f, height(lastScale_) - 18.0f * lastScale_);
    float v = std::clamp(1.0f - (py - y) / barH, 0.0f, 1.0f);
    if (v == value_) return false;
    value_ = v;
    if (onChange_) onChange_(value_);
    return true;
}

bool TouchBar::pointer(const PointerEvent& e, float x, float y, float w) {
    switch (e.action) {
    case PointerAction::Down:
        if (owner_ != kNone || e.button != 1 || !inside(e, x, y, w, height(lastScale_))) return false;
        owner_ = e.pointerId;
        setFrom(e.y, y);
        return true;
    case PointerAction::Move:
        return owner_ == e.pointerId && setFrom(e.y, y);
    case PointerAction::Up:
        if (owner_ != e.pointerId) return false;
        cancel();
        return true;
    case PointerAction::Leave:
    case PointerAction::Wheel:
        return false;
    }
    return false;
}

void TouchBar::cancel() {
    if (owner_ == kNone) return;
    owner_ = kNone;
    value_ = 0.0f;
    if (onChange_) onChange_(0.0f);
}

std::vector<Placement> WidgetPanel::arrange(float width, float height, float scale) {
    (void)height;
    float pad = 12.0f * scale;
    float w = std::max(0.0f, width - 2.0f * pad);
    std::vector<Placement> out;
    float y = pad;
    for (const std::unique_ptr<Widget>& widget : widgets_) {
        out.push_back(Placement{pad, y, w});
        y += widget->height(scale) + 8.0f * scale;
    }
    return out;
}

void WidgetPanel::draw(DrawContext& ctx) {
    scale_ = ctx.scale;
    placements_ = arrange(static_cast<float>(ctx.width), static_cast<float>(ctx.height), ctx.scale);
    for (std::size_t i = 0; i < widgets_.size() && i < placements_.size(); ++i)
        widgets_[i]->draw(ctx, placements_[i].x, placements_[i].y, placements_[i].w);
}

bool WidgetPanel::pointer(const PointerEvent& e) {
    if (placements_.size() != widgets_.size()) return false;
    std::vector<std::pair<std::uint64_t, Widget*>>::iterator held =
        std::find_if(captured_.begin(), captured_.end(),
                     [&](const std::pair<std::uint64_t, Widget*>& c) { return c.first == e.pointerId; });
    bool changed = false;
    for (std::size_t i = 0; i < widgets_.size(); ++i) {
        Widget* w = widgets_[i].get();
        const Placement& p = placements_[i];
        if (held != captured_.end() && w != held->second && e.action != PointerAction::Leave) continue;
        if (e.action == PointerAction::Down && held == captured_.end()) {
            bool hit = e.x >= p.x && e.x < p.x + p.w && e.y >= p.y && e.y < p.y + w->height(scale_);
            if (!hit) continue;
            captured_.emplace_back(e.pointerId, w);
            held = captured_.end() - 1;
        }
        changed = w->pointer(e, p.x, p.y, p.w) || changed;
    }
    if (e.action == PointerAction::Up && held != captured_.end()) captured_.erase(held);
    return changed;
}

void WidgetPanel::focusLost() {
    for (const std::unique_ptr<Widget>& w : widgets_) w->cancel();
    captured_.clear();
}

}
