// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// xr-pilot: an OXRSys client a person and an agent both drive. Commands arrive on stdin and replies
// leave on stdout, one per line (core/include/xrpilot/Commands.h); the keyboard and mouse drive the
// same state through HumanInput. Decode, present and screenshots run on the window's thread and device.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "panelspun/widgets.h"
#include "panelspun/window.h"

#include "xrpilot/Client.h"
#include "xrpilot/CommandLoop.h"
#include "xrpilot/Commands.h"
#include "xrpilot/GpuDecoder.h"
#include "xrpilot/HumanInput.h"
#include "xrpilot/Launch.h"
#include "xrpilot/SpanLog.h"
#include "xrpilot/TraceStore.h"
#include "xrpilot/Png.h"
#include "xrpilot/HeadGizmo.h"
#include "xrpilot/Pump.h"
#include "xrpilot/Sparkline.h"
#include "xrpilot/StreamRecord.h"
#include "xrpilot/Theme.h"
#include "xrpilot/Tray.h"
#include "xrpilot/UiTrace.h"
#include "xrpilot/Version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace panelspun;
using namespace xrpilot;

namespace
{

constexpr int EscapeScancode = 41;
constexpr int PScancode = 19;

struct Shared
{
    std::mutex mutex;
    std::condition_variable done;
    std::string screenshotPath; // pending request, served on the window thread
    std::string screenshotReply;
    std::atomic<uint64_t> decoded{0};
    std::atomic<uint64_t> decodeErrors{0};
    std::atomic<bool> captured{false};
    SpanLog spans;
    TraceStore traces; // every span, kept in SQLite
    UiTrace ui{&traces}; // every UI action, as a span of traces (UiSpans.h, from lean/XrPilot/Ui.lean)
    std::string snapshotPath; // --snapshot: the 90th decoded frame
    StreamRecordWriter stream; // --record-stream: every frame as it arrived, before decode
};

int64_t nowNs()
{
    return monotonicNowNs();
}

// The left eye, and the person's keyboard and mouse: a left click captures the mouse and a left drag
// looks around; captured, left click pulls the trigger; right click toggles capture; Escape or
// leaving the window lets go.
class EyePanel final : public Panel
{
public:
    EyePanel(Client& client, Shared& shared, Window& window)
        : Panel("Left eye")
        , client_(client)
        , shared_(shared)
        , window_(window)
    {
    }

    bool usesVulkanRegion() const override { return true; }
    void draw(DrawContext&) override {}

    bool pointer(const PointerEvent& e) override
    {
        const bool captured = shared_.captured.load();
        if (e.action == PointerAction::Down)
        {
            if (e.button == SDL_BUTTON_RIGHT)
                setCaptured(!captured);
            else if (e.button == SDL_BUTTON_LEFT && !captured)
            {
                dragging_ = true;
                dragDistance_ = 0.0f;
            }
            else if (e.button == SDL_BUTTON_LEFT)
                press(key::TriggerMouse);
            else if (e.button == SDL_BUTTON_MIDDLE)
                press(key::HeadsetButton);
        }
        else if (e.action == PointerAction::Up)
        {
            if (e.button == SDL_BUTTON_LEFT && dragging_)
            {
                dragging_ = false;
                if (dragDistance_ < 4.0f)
                    setCaptured(true);
            }
            else if (e.button == SDL_BUTTON_LEFT)
                release(key::TriggerMouse);
            else if (e.button == SDL_BUTTON_MIDDLE)
            {
                release(key::HeadsetButton);
            }
        }
        else if (e.action == PointerAction::Move && (captured || dragging_))
        {
            // The tracking thread turns the head, at the time the mouse moved (SDL's event timestamp
            // moved onto monotonicNowNs's clock), not when this window frame read it.
            const int64_t arrived = monotonicNowNs();
            const int64_t at = e.timestampNs != 0 ? arrived - int64_t(SDL_GetTicksNS() - e.timestampNs) : arrived;
            client_.pushMouse(std::min(at, arrived), arrived, e.dx, e.dy);
            dragDistance_ += std::abs(e.dx) + std::abs(e.dy);
        }
        else if (e.action == PointerAction::Wheel)
        {
            wheel_ += e.dy;
        }
        return false;
    }

    bool key(const KeyEvent& e) override
    {
        if (e.repeat)
            return false;
        if (e.down && e.scancode == EscapeScancode)
        {
            setCaptured(false);
            return false;
        }
        if (e.down && e.scancode == PScancode)
        {
            client_.updateAgent([](AgentState& s) { s.pointing = false; });
            return false;
        }
        if (e.down)
            press(e.scancode);
        else
            release(e.scancode);
        return false;
    }

    void focusLost() override
    {
        shared_.captured = false;
        dragging_ = false;
        client_.clearMouse();
        keys_.clear();
        pressedThisTick_.clear();
        releaseNextTick_.clear();
    }

    // Decode, screenshots and the person's input run here rather than in recordVulkan so they
    // continue while minimized.
    void update(const VulkanContext& vk) override
    {
        applyHumanInput();
        if (!ready_ && !failed_)
        {
            GpuContext context{vk.getInstanceProcAddr, vk.instance, vk.physicalDevice, vk.device, vk.queue,
                               vk.queueFamily,         vk.instanceInfo, vk.deviceInfo};
            std::string error;
            ready_ = decoder_.initialize(context, &error);
            failed_ = !ready_;
            if (failed_)
                std::fprintf(stderr, "xr-pilot: %s\n", error.c_str());
        }
        if (!ready_)
            return;
        const ClientStatus status = client_.status();
        if (status.framesDropped > lastDropped_)
            client_.requestKeyframe(oxr::protocol::KEYFRAME_REASON_FRAME_LOSS, uint32_t(status.framesDropped - lastDropped_));
        lastDropped_ = status.framesDropped;
        if (std::optional<AssembledVideoFrame> next = client_.takeFrame())
        {
            if (shared_.stream.isOpen())
                shared_.stream.write({next->presentationTimeNs, next->receiveTimeNs, next->frameIndex, next->nalUnit});
            const int64_t start = nowNs();
            if (decoder_.decode(next->nalUnit.data(), next->nalUnit.size()))
            {
                client_.reportLatency(*next, start, nowNs());
                consecutiveErrors_ = 0;
                lastStreamed_ = std::move(next->nalUnit);
                if (++shared_.decoded == 90 && !shared_.snapshotPath.empty())
                    writeSnapshot(shared_.snapshotPath);
            }
            else
            {
                ++shared_.decodeErrors;
                if (++consecutiveErrors_ >= 3)
                {
                    client_.requestKeyframe(oxr::protocol::KEYFRAME_REASON_DECODE_STALL, uint32_t(consecutiveErrors_));
                    consecutiveErrors_ = 0;
                }
            }
        }
        serveScreenshot();
        serveTrace();
    }

    void recordVulkan(const VulkanRegionFrame& frame) override
    {
        if (ready_)
            decoder_.recordLeftEye(frame.commands, frame.image, frame.width, frame.height, false);
    }

private:
    void press(int code)
    {
        keys_.insert(code);
        pressedThisTick_.insert(code);
        releaseNextTick_.erase(code);
    }

    // A tap shorter than a tick still reaches one packet.
    void release(int code)
    {
        if (pressedThisTick_.count(code) != 0)
            releaseNextTick_.insert(code);
        else
            keys_.erase(code);
    }

    void setCaptured(bool on)
    {
        window_.setRelativeMouse(on);
        shared_.captured = on && window_.relativeMouse();
        if (!shared_.captured)
        {
            keys_.erase(key::TriggerMouse);
            client_.clearMouse();
        }
    }

    void applyHumanInput()
    {
        const std::set<int> keys = keys_;
        const float wheel = wheel_;
        wheel_ = 0.0f;
        client_.updateAgent([&](AgentState& s) {
            s.keys = keys;
            // Mouse look and held keys act on the client's tracking thread; this only raises pointing.
            advanceHuman(s, 0.0f, 0.0f, 0.0f);
            applyWheel(s, wheel);
        });
        pressedThisTick_.clear();
        for (int code : releaseNextTick_)
            keys_.erase(code);
        releaseNextTick_.clear();
    }

    void writeSnapshot(const std::string& path)
    {
        std::vector<uint8_t> rgba;
        int w = 0;
        int h = 0;
        const bool ok = decoder_.snapshotLeftEye(rgba, w, h) && writePng(path, rgba.data(), w, h);
        std::fprintf(stderr, "xr-pilot: snapshot %s %s\n", path.c_str(), ok ? "written" : "failed");
    }

    // A span that began or ended takes the frame on screen now as its input or its edit.
    void serveTrace()
    {
        if (!shared_.traces.wantsFrame())
            return;
        TraceFrame frame;
        if (!decoder_.snapshotLeftEye(frame.rgba, frame.width, frame.height))
            return;
        frame.sequence = shared_.decoded.load();
        if (GpuDecoder::frameSize(lastStreamed_.data(), lastStreamed_.size(), frame.encodedWidth, frame.encodedHeight))
            frame.encoded = lastStreamed_;
        const AgentState agent = client_.agent();
        for (const Pose* pose : {&agent.head, &agent.hands[0].pose, &agent.hands[1].pose})
        {
            frame.pose.insert(frame.pose.end(), pose->position, pose->position + 3);
            frame.pose.insert(frame.pose.end(), pose->rotation.m, pose->rotation.m + 9);
        }
        shared_.traces.frame(std::move(frame));
    }

    void serveScreenshot()
    {
        std::lock_guard<std::mutex> lock(shared_.mutex);
        if (shared_.screenshotPath.empty())
            return;
        std::vector<uint8_t> rgba;
        int w = 0;
        int h = 0;
        if (!decoder_.snapshotLeftEye(rgba, w, h))
            shared_.screenshotReply = errorJson("no decoded frame yet");
        else if (!writePng(shared_.screenshotPath, rgba.data(), w, h))
            shared_.screenshotReply = errorJson("cannot write " + shared_.screenshotPath);
        else
            shared_.screenshotReply = "{\"ok\":true,\"path\":" + jsonString(shared_.screenshotPath) + ",\"width\":" +
                                      std::to_string(w) + ",\"height\":" + std::to_string(h) + "}";
        shared_.screenshotPath.clear();
        shared_.done.notify_all();
    }

    Client& client_;
    Shared& shared_;
    Window& window_;
    GpuDecoder decoder_;
    std::vector<uint8_t> lastStreamed_; // the frame on screen, as the runtime streamed it
    bool ready_ = false;
    bool failed_ = false;
    std::set<int> keys_;
    std::set<int> pressedThisTick_;
    std::set<int> releaseNextTick_;
    float wheel_ = 0.0f;
    bool dragging_ = false;
    float dragDistance_ = 0.0f;
    uint64_t lastDropped_ = 0;
    int consecutiveErrors_ = 0;
};

// The palette every widget draws with; switched on the UI thread with the window's theme.
const Palette* currentPalette = &palette(Scheme::Dark);

Color colour(Rgb c, std::uint8_t a = 255)
{
    return Color{c.r, c.g, c.b, a};
}

Color blend(Rgb a, Rgb b, float t)
{
    const auto mix = [t](std::uint8_t x, std::uint8_t y) { return std::uint8_t(std::lround(x + (y - x) * t)); };
    return Color{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), 255};
}

Theme themeFrom(const Palette& p)
{
    Theme t;
    t.background = colour(p.background);
    t.panel = colour(p.panel);
    t.header = colour(p.header);
    t.handle = colour(p.handle);
    t.handleActive = colour(p.accent);
    t.text = colour(p.text);
    t.accent = colour(p.accent);
    t.control = colour(p.control);
    t.controlHover = colour(p.controlHover);
    return t;
}

bool systemDark()
{
    return SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT;
}

// One labelled sparkline row.
class SparkRow final : public Widget
{
public:
    SparkRow(std::string label, bool faultSeries)
        : label_(std::move(label))
        , fault_(faultSeries)
    {
    }

    void sample(uint64_t total, bool expecting) { line_.sample(total, expecting); }
    float height(float scale) const override { return 14.0f * scale; }

    void draw(DrawContext& ctx, float x, float y, float w) override
    {
        const float labelWidth = 44.0f * ctx.scale;
        const float h = height(ctx.scale);
        Label label(label_, 12.0f);
        label.draw(ctx, x, y + (h - label.height(ctx.scale)) * 0.5f, labelWidth);
        const float left = x + labelWidth;
        const float width = std::max(0.0f, w - labelWidth);
        const float base[4] = {left, y + h - 1.0f, left + width, y + h - 1.0f};
        drawPolyline(ctx, base, 2, colour(currentPalette->rule), 1.0f);
        // Once-a-second ticks scroll with the samples, so a strip of zeros still visibly moves.
        for (float tx : line_.ticks(width))
        {
            const float tick[4] = {left + tx, y + h - 1.0f, left + tx, y + h - 4.0f * ctx.scale};
            drawPolyline(ctx, tick, 2, colour(currentPalette->rule), 1.0f);
        }
        // The box's bottom edge is the base rule; zero sits a pixel above it.
        const std::vector<Sparkline::Segment> segments = line_.segments(width, h - 2.0f, fault_);
        for (const Sparkline::Segment& s : segments)
        {
            const Palette& p = *currentPalette;
            const Color c = colour(s.colour == Sparkline::Colour::Fault     ? p.fault
                                   : s.colour == Sparkline::Colour::Clipped ? p.warning
                                                                            : p.good);
            // A fault is drawn thicker too, so it reads without its colour.
            const float thick = s.colour == Sparkline::Colour::Fault ? 2.5f : 1.25f;
            const float xy[4] = {left + s.x0, y + 1.0f + s.y0, left + s.x1, y + 1.0f + s.y1};
            drawPolyline(ctx, xy, 2, c, thick * ctx.scale);
        }
        if (!segments.empty())
        {
            // The newest sample, at the right edge.
            const Sparkline::Segment& s = segments.back();
            const float r = 1.5f * ctx.scale;
            const float dot[4] = {left + s.x1 - r, y + 1.0f + s.y1, left + s.x1 + r, y + 1.0f + s.y1};
            drawPolyline(ctx, dot, 2, colour(currentPalette->text), 2.0f * r);
        }
    }

private:
    std::string label_;
    bool fault_;
    Sparkline line_;
};

// Where the head looks, as a sphere seen from above right: orange is the yaw from forward along the
// horizon, sky blue the pitch up or down from it, green the look ray; the X, Y and Z axes are labelled
// in Godot's red, green and blue,
// and the legend under it states the conventions the pilot and the runtime share.
class HeadGizmoWidget final : public Widget
{
public:
    void set(const Rotation& head) { gizmo_ = buildHeadGizmo(head); }
    void showConventions(bool on) { conventions_ = on; }
    bool conventions() const { return conventions_; }
    float height(float scale) const override { return (120.0f + (conventions_ ? 58.0f : 0.0f)) * scale; }

    void draw(DrawContext& ctx, float x, float y, float) override
    {
        const Palette& p = *currentPalette;
        const float size = 120.0f * ctx.scale;
        float tips[3][2] = {};
        for (const HeadGizmo::Segment& s : gizmo_.segments(size))
        {
            Rgb c = p.rule;
            float width = 1.0f;
            switch (s.role)
            {
            case HeadGizmo::Role::Outline: c = p.rule; break;
            case HeadGizmo::Role::Back: c = p.control; break;
            case HeadGizmo::Role::Horizon: c = p.muted; break;
            case HeadGizmo::Role::Pole: c = p.text; width = 2.0f; break;
            case HeadGizmo::Role::Yaw: c = p.warning; width = 2.5f; break;
            case HeadGizmo::Role::Pitch: c = p.info; width = 2.5f; break;
            case HeadGizmo::Role::Look: c = p.good; width = 2.0f; break;
            case HeadGizmo::Role::AxisX: c = p.axisX; width = 1.5f; tips[0][0] = s.x1; tips[0][1] = s.y1; break;
            case HeadGizmo::Role::AxisY: c = p.axisY; width = 1.5f; tips[1][0] = s.x1; tips[1][1] = s.y1; break;
            case HeadGizmo::Role::AxisZ: c = p.axisZ; width = 1.5f; tips[2][0] = s.x1; tips[2][1] = s.y1; break;
            }
            const float xy[4] = {x + s.x0, y + s.y0, x + s.x1, y + s.y1};
            drawPolyline(ctx, xy, 2, colour(c), width * ctx.scale);
        }
        const char* names[3] = {"+X", "+Y", "+Z"};
        for (int a = 0; a < 3; ++a)
            Label(names[a], 10.0f).draw(ctx, x + tips[a][0] + 2.0f * ctx.scale, y + tips[a][1] - 6.0f * ctx.scale, 24.0f * ctx.scale);
        Label text("yaw " + std::to_string(int(std::lround(gizmo_.yawDegrees))) + "  pitch " +
                       std::to_string(int(std::lround(gizmo_.pitchDegrees))),
                   11.0f);
        text.draw(ctx, x + size + 8.0f * ctx.scale, y + (size - text.height(ctx.scale)) * 0.5f, 140.0f * ctx.scale);
        if (!conventions_)
            return;
        // Read from the code: HeadGizmo and Rotation (-Z forward, +Y up), the body overlay (floor y = 0),
        // the hologram rasterizer (no culling) and the screenshot pixels point_at and click take.
        float ly = y + size + 4.0f * ctx.scale;
        for (const char* line : {"Godot axes: UP +Y (floor y = 0), FORWARD -Z, RIGHT +X, right-handed",
                                 "Front faces CCW as glTF and OpenXR (Godot ArrayMesh: CW)",
                                 "Screenshot pixels: origin top left, x right, y down"})
        {
            Label l(line, 10.5f);
            l.draw(ctx, x, ly, 0.0f);
            ly += 18.0f * ctx.scale;
        }
    }

private:
    HeadGizmo gizmo_ = buildHeadGizmo(Rotation{});
    bool conventions_ = true;
};

// The GUI's colour checker: the 24 patches of a Macbeth ColorChecker Classic in X-Rite's published sRGB,
// so a wrong swapchain gamma or format shows against a physical chart, then each palette role with its
// WCAG contrast against the surface it sits on (4.5:1 for text, 3:1 for marks). The rendered view is the
// content's to check, not ours.
class ColourCheckerWidget final : public Widget
{
public:
    void show(bool on) { shown_ = on; }
    float height(float scale) const override
    {
        return shown_ ? (4.0f * 20.0f + 6.0f + float(roles().size()) * 16.0f) * scale : 0.0f;
    }

    void draw(DrawContext& ctx, float x, float y, float w) override
    {
        if (!shown_)
            return;
        const Palette& p = *currentPalette;
        const float s = ctx.scale;
        const float patch = std::min(w / 6.0f, 30.0f * s);
        for (int i = 0; i < 24; ++i)
            drawRect(ctx, x + float(i % 6) * patch, y + float(i / 6) * 20.0f * s, patch - 3.0f * s, 17.0f * s, 2.0f * s,
                     colour(Macbeth[i]));
        y += (4.0f * 20.0f + 6.0f) * s;
        for (const Role& r : roles())
        {
            const Rgb fg = r.surface ? p.text : p.*r.colour;
            const Rgb bg = r.surface ? p.*r.colour : p.panel;
            const double ratio = contrast(fg, bg);
            const bool pass = ratio >= r.needs;
            const float mid = y + 8.0f * s;
            drawRect(ctx, x, y + 2.0f * s, 24.0f * s, 12.0f * s, 2.0f * s, colour(p.*r.colour));
            if (r.surface)
                drawText(ctx, "Aa", 10.0f, x + 12.0f * s, mid, 0.5f, colour(p.text));
            char ratioText[16];
            std::snprintf(ratioText, sizeof(ratioText), "%.1f:1", ratio);
            const Color c = colour(pass ? p.text : p.faultText);
            drawText(ctx, r.name, 13.0f, x + 32.0f * s, mid, 0.0f, c);
            drawText(ctx, ratioText, 13.0f, x + w * 0.62f, mid, 1.0f, c);
            drawText(ctx, std::string(pass ? "pass " : "FAIL ") + (r.needs > 4.0 ? "text" : "mark"), 13.0f,
                     x + w * 0.66f, mid, 0.0f, c);
            y += 16.0f * s;
        }
    }

private:
    static constexpr Rgb Macbeth[24] = {
        {115, 82, 68},   {194, 150, 130}, {98, 122, 157},  {87, 108, 67},   {133, 128, 177}, {103, 189, 170},
        {214, 126, 44},  {80, 91, 166},   {193, 90, 99},   {94, 60, 108},   {157, 188, 64},  {224, 163, 46},
        {56, 61, 150},   {70, 148, 73},   {175, 54, 60},   {231, 199, 31},  {187, 86, 149},  {8, 133, 161},
        {243, 243, 242}, {200, 200, 200}, {160, 160, 160}, {122, 122, 121}, {85, 85, 85},    {52, 52, 52},
    };
    struct Role
    {
        const char* name;
        Rgb Palette::*colour;
        double needs;
        bool surface; // a surface is checked by the text drawn on it
    };
    static const std::vector<Role>& roles()
    {
        static const std::vector<Role> all{
            {"background", &Palette::background, 4.5, true}, {"panel", &Palette::panel, 4.5, true},
            {"header", &Palette::header, 4.5, true},         {"control", &Palette::control, 4.5, true},
            {"controlHover", &Palette::controlHover, 4.5, true}, {"muted", &Palette::muted, 4.5, false},
            {"faultText", &Palette::faultText, 4.5, false},  {"accent", &Palette::accent, 3.0, false},
            {"rule", &Palette::rule, 3.0, false},            {"good", &Palette::good, 3.0, false},
            {"warning", &Palette::warning, 3.0, false},      {"fault", &Palette::fault, 3.0, false},
            {"info", &Palette::info, 3.0, false},            {"axisX", &Palette::axisX, 3.0, false},
            {"axisY", &Palette::axisY, 3.0, false},          {"axisZ", &Palette::axisZ, 3.0, false},
        };
        return all;
    }
    bool shown_ = false;
};

// The agent's tool calls as a trace: one row per span with its status, name and duration, newest at
// the bottom with the pilot commands it sent beneath it.
class TracePanel final : public Panel
{
public:
    explicit TracePanel(Shared& shared)
        : Panel("Trace")
        , shared_(shared)
    {
    }

    void draw(DrawContext& ctx) override
    {
        const std::vector<Span> spans = shared_.spans.visible();
        rows_.clear();
        const int64_t now = nowNs() / 1'000'000;
        const float s = ctx.scale;
        const float pad = 10.0f * s;
        const float row = 22.0f * s;
        const float sub = 17.0f * s;
        const float nameX0 = pad + 16.0f * s;
        // Names take the width; the duration is its time, dim, at the right edge.
        const float msW = 40.0f * s;
        const float barX = ctx.width - pad - msW;
        const Palette& p = *currentPalette;
        float y = ctx.height - pad;
        int age = 0;
        for (std::vector<Span>::const_reverse_iterator it = spans.rbegin(); it != spans.rend() && y > 0.0f; ++it, ++age)
        {
            if (age == 0)
            {
                const size_t shown = std::min<size_t>(it->children.size(), 4);
                const float nameX = nameX0 + 14.0f * s * float(std::min(it->depth, 6));
                for (size_t c = it->children.size() - shown; c < it->children.size(); ++c)
                {
                    y -= sub;
                    const SpanChild& child = it->children[c];
                    const std::string text = fit(child.text, ctx.width - nameX - 10.0f * s - pad, 11.0f, s);
                    tinted(ctx, colour(child.ok ? p.muted : p.faultText), [&](DrawContext& t) {
                        Label(text, 11.0f).draw(t, nameX + 10.0f * s, y, 0.0f);
                    });
                }
            }
            y -= row;
            rows_.push_back(Row{y, y + row, it->id});
            const float indent = 14.0f * s * float(std::min(it->depth, 6));
            const float nameX = nameX0 + indent;
            // Older rows fade toward the muted text colour, which still reads at AA on the panel.
            const float fade = std::min(1.0f, 0.08f * float(age));
            const Rgb status = it->running() ? p.info : it->ok ? p.good : p.fault;
            const float mid = y + row * 0.5f;
            const float cx = pad + indent + 4.0f * s;
            if (!it->running() && !it->ok)
            {
                // A failure is a cross, not only a different colour.
                const float r = 4.0f * s;
                const float a[4] = {cx - r, mid - r, cx + r, mid + r};
                const float b[4] = {cx - r, mid + r, cx + r, mid - r};
                drawPolyline(ctx, a, 2, colour(status), 2.0f * s);
                drawPolyline(ctx, b, 2, colour(status), 2.0f * s);
            }
            else
            {
                const float dot[4] = {cx, mid, cx + 0.1f, mid};
                drawPolyline(ctx, dot, 2, colour(status), (it->running() ? 5.0f : 8.0f) * s);
            }
            std::string name = it->name + (it->count > 1 ? "  x" + std::to_string(it->count) : "");
            if (!it->running() && it->nested > 0)
                name = std::string(it->expanded ? "v " : "> ") + name + "  (" + std::to_string(it->nested) + ")";
            name = fit(name, barX - 8.0f * s - nameX, 13.0f, s);
            tinted(ctx, blend(p.text, p.muted, fade),
                   [&](DrawContext& t) { Label(name, 13.0f).draw(t, nameX, y, 0.0f); });
            const int64_t ms = it->durationMs(now);
            tinted(ctx, colour(p.muted), [&](DrawContext& t) {
                const std::string when = ms < 1000 ? std::to_string(ms) + "ms" : std::to_string(ms / 1000) + "." +
                                                                                     std::to_string(ms % 1000 / 100) + "s";
                Label(when, 11.0f).draw(t, barX, y, 0.0f);
            });
        }
    }

    // A click on a finished span's row expands or folds what ran inside it.
    bool pointer(const PointerEvent& e) override
    {
        if (e.action != PointerAction::Down || e.button != 1)
            return false;
        for (const Row& r : rows_)
        {
            if (e.y >= r.top && e.y < r.bottom)
            {
                shared_.spans.toggle(r.id);
                return true;
            }
        }
        return false;
    }

private:
    struct Row
    {
        float top;
        float bottom;
        std::string id;
    };
    std::vector<Row> rows_;

    // The text cut to fit width at the UI font's average glyph width, so a label never runs into the bar.
    static std::string fit(const std::string& text, float width, float points, float scale)
    {
        const size_t room = size_t(std::max(0.0f, width / (0.58f * points * scale)));
        if (text.size() <= room)
            return text;
        return room > 3 ? text.substr(0, room - 3) + "..." : std::string();
    }

    static void tinted(DrawContext& ctx, Color c, const std::function<void(DrawContext&)>& draw)
    {
        Theme theme = *ctx.theme;
        theme.text = c;
        DrawContext t = ctx;
        t.theme = &theme;
        draw(t);
    }

    Shared& shared_;
};

// The stream's counters as a status bar of sparklines side by side, the last 10 s. A pump samples them
// every 250 ms on its own thread, so they keep moving while the window stalls, is minimized or lost its
// device; pkt and fps draw red while connected and silent (lean/XrPilot/Spark.lean).
class StatsPanel final : public Panel
{
public:
    StatsPanel(Client& client, Shared& shared, Window& window)
        : Panel("Stream")
        , client_(client)
        , shared_(shared)
        , rows_{SparkRow("pkt", false), SparkRow("fps", false), SparkRow("drop", true), SparkRow("fec", true),
                SparkRow("err", true)}
        , pump_(250'000'000, [this, &window] {
            sample();
            window.requestRedrawFromAnyThread();
        })
    {
    }

    void draw(DrawContext& ctx) override
    {
        const float pad = 12.0f * ctx.scale;
        const float gap = 16.0f * ctx.scale;
        const float width = (ctx.width - 2.0f * pad - 4.0f * gap) / 5.0f;
        if (width <= 0.0f)
            return;
        const float y = (ctx.height - rows_[0].height(ctx.scale)) * 0.5f;
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < 5; ++i)
            rows_[i].draw(ctx, pad + float(i) * (width + gap), y, width);
    }

private:
    // On the pump's thread. pkt and fps expect data while connected; the fault series count faults.
    void sample()
    {
        const ClientStatus s = client_.status();
        const uint64_t totals[5] = {s.videoPackets, shared_.decoded.load(), s.framesDropped, s.fecRecoveries,
                                    shared_.decodeErrors.load()};
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < 5; ++i)
            rows_[i].sample(totals[i], i < 2 && s.connected);
    }

    Client& client_;
    Shared& shared_;
    std::mutex mutex_;
    SparkRow rows_[5];
    // Last, so it stops before the rows it samples go.
    Pump pump_;
};

std::string fixed(float value, int digits)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", digits, value);
    return text;
}

// The simulator's left column: connection, field of view, controllers, pointing and the keys.
class ControlsPanel final : public WidgetPanel
{
public:
    ControlsPanel(Client& client, Shared& shared, Tray* tray, Window* window)
        : WidgetPanel("XR Pilot")
        , window_(window)
        , tray_(tray)
        , client_(client)
        , shared_(shared)
    {
        connection_ = add(std::make_unique<Label>("Searching for an OXRSys runtime"));
        runtime_ = add(std::make_unique<Label>(""));
        add(std::make_unique<Button>("Connect", [this] {
            shared_.ui.record(UiAction::Connect);
            client_.connectNow();
        }));
        add(std::make_unique<Button>("Disconnect", [this] {
            shared_.ui.record(UiAction::Disconnect);
            client_.disconnect();
        }));
        autoConnect_ = add(std::make_unique<Toggle>("Connect automatically", true,
                                                    [this](bool on) {
                                                        shared_.ui.record(UiAction::AutoConnect, true, on ? "on" : "off");
                                                        client_.setAutoConnect(on);
                                                    }));
        fovLabel_ = add(std::make_unique<Label>("Vertical FOV 100 deg"));
        add(std::make_unique<Slider>(60.0f, 150.0f, 100.0f, [this](float v) {
            // A drag moves the slider every frame; a span per whole degree keeps the trace readable.
            if (std::fabs(v - lastFovTraced_) >= 1.0f)
            {
                lastFovTraced_ = v;
                shared_.ui.record(UiAction::FieldOfView, true, std::to_string(int(std::lround(v))));
            }
            client_.updateAgent([v](AgentState& s) { s.verticalFovDegrees = v; });
        }));
        controllers_ = add(std::make_unique<Toggle>("Controllers", true, [this](bool on) {
            shared_.ui.record(UiAction::Controllers, true, on ? "on" : "off");
            client_.updateAgent([on](AgentState& s) {
                s.hands[0].present = on;
                s.hands[1].present = on;
            });
        }));
        pointing_ = add(std::make_unique<Toggle>("Pointing (P)", false, [this](bool on) {
            shared_.ui.record(UiAction::Pointing, true, on ? "on" : "off");
            client_.updateAgent([on](AgentState& s) {
                s.pointing = on;
                s.pointingAge = 0.0f;
            });
        }));
        seated_ = add(std::make_unique<Toggle>("Seated", false, [this](bool on) {
            shared_.ui.record(UiAction::Seated, true, on ? "on" : "off");
            client_.updateAgent([on](AgentState& s) { setSeated(s, on); });
        }));
        pose_ = add(std::make_unique<Label>(""));
        add(std::make_unique<Toggle>("Conventions", true, [this](bool on) {
            shared_.ui.record(UiAction::Conventions, true, on ? "on" : "off");
            gizmo_->showConventions(on);
        }));
        gizmo_ = add(std::make_unique<HeadGizmoWidget>());
        appearance_ = add(std::make_unique<Label>(""));
        add(std::make_unique<Button>("Switch appearance", [this] {
            appearanceChoice_ = nextAppearance(appearanceChoice_);
            shared_.ui.record(UiAction::Appearance, true, appearanceName(appearanceChoice_));
            applyScheme();
        }));
        add(std::make_unique<Toggle>("Colour checker", false, [this](bool on) {
            shared_.ui.record(UiAction::ColourChecker, true, on ? "on" : "off");
            checker_->show(on);
        }));
        checker_ = add(std::make_unique<ColourCheckerWidget>());
        capture_ = add(std::make_unique<Label>(""));
        for (const char* help : {"Drag the view to look; click to capture, Esc lets go",
                                 "WASD walk, wheel sets its speed, E/R roll",
                                 "Shift moves a hand, Shift+wheel its reach",
                                 "T/H/click triggers, F/G grips, 1-4 XYAB, M menu, P lowers"})
            add(std::make_unique<Label>(help, 11.0f));
    }

    void update(const VulkanContext&) override
    {
        const int64_t now = nowNs();
        if (now - lastTrayNs_ >= 2'000'000'000)
        {
            lastTrayNs_ = now;
            if (appearanceChoice_ == Appearance::Auto)
                applyScheme();
            if (tray_ != nullptr)
            {
                tray_->poll();
            }
            // The tray's Stream to a headset mode: the headset owns the session, and this pilot watches.
            if (client_.spectator() != Tray::streamsToHeadset())
            {
                client_.setSpectator(Tray::streamsToHeadset());
            }
        }
    }

    void draw(DrawContext& ctx) override
    {
        const ClientStatus s = client_.status();
        const AgentState a = client_.agent();
        connection_->setText(s.connected    ? "Streaming from " + s.serverName
                             : s.discovered ? "Found " + s.serverName + ", not connected"
                                            : "Searching for an OXRSys runtime");
        runtime_->setText(s.discovered ? s.server + "  " + std::to_string(s.renderWidth) + "x" +
                                             std::to_string(s.renderHeight) + " per eye  " +
                                             std::to_string(s.refreshHz) + " Hz"
                                       : "");
        autoConnect_->setValue(s.autoConnect);
        float halfH = 0.0f;
        float halfV = 0.0f;
        eyeHalfFov(a, halfH, halfV);
        const bool runtimeFov = a.renderTangents[1] > a.renderTangents[0];
        fovLabel_->setText("Vertical FOV " + std::to_string(int(std::lround(2.0f * halfV * 57.29578f))) + " deg" +
                           (runtimeFov ? ", set by the runtime" : ""));
        controllers_->setValue(a.hands[1].present);
        pointing_->setValue(a.pointing);
        seated_->setValue(a.seated);
        float yaw, pitch, roll;
        toYawPitchRoll(a.head.rotation, yaw, pitch, roll);
        pose_->setText("Walk " + fixed(a.moveSpeed, 2) + " m/s  head yaw " + fixed(yaw, 0) + "  pitch " + fixed(pitch, 0) + "  at " +
                       fixed(a.head.position[0], 2) + ", " + fixed(a.head.position[1], 2) + ", " +
                       fixed(a.head.position[2], 2));
        gizmo_->set(a.head.rotation);
        capture_->setText(shared_.captured ? "Mouse captured" : "Mouse free");
        appearance_->setText(std::string("Appearance ") + appearanceName(appearanceChoice_) + ", " +
                             (scheme_ == Scheme::Dark ? "dark" : "light"));
        WidgetPanel::draw(ctx);
    }

private:
    // Auto follows the system's light or dark setting, polled with the tray.
    void applyScheme()
    {
        const Scheme scheme = schemeFor(appearanceChoice_, systemDark());
        if (scheme == scheme_)
            return;
        scheme_ = scheme;
        currentPalette = &palette(scheme);
        if (window_ != nullptr)
            window_->setTheme(themeFrom(*currentPalette));
    }

    Window* window_ = nullptr;
    Appearance appearanceChoice_ = Appearance::Auto;
    Scheme scheme_ = systemDark() ? Scheme::Dark : Scheme::Light;
    Label* appearance_ = nullptr;
    ColourCheckerWidget* checker_ = nullptr;
    Client& client_;
    Shared& shared_;
    Label* connection_ = nullptr;
    Label* runtime_ = nullptr;
    Toggle* autoConnect_ = nullptr;
    float lastFovTraced_ = 100.0f;
    Label* fovLabel_ = nullptr;
    Toggle* controllers_ = nullptr;
    Toggle* pointing_ = nullptr;
    Toggle* seated_ = nullptr;
    Label* pose_ = nullptr;
    HeadGizmoWidget* gizmo_ = nullptr;
    Label* capture_ = nullptr;
    Tray* tray_ = nullptr;
    int64_t lastTrayNs_ = 0;
};

// xr-pilot.exe is a Windows GUI app, so stderr goes nowhere: a person sees errors in a box. An agent
// reads stderr, so it gets them there alone.
void reportError(bool agent, const std::string& message)
{
    std::fprintf(stderr, "xr-pilot: %s\n", message.c_str());
    if (!agent)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "XR Pilot", message.c_str(), nullptr);
}

int runPilot(Shared& shared, bool autoConnect, bool agent, Tray& tray, bool readCommands,
             CommandBridge* bridge = nullptr)
{
    Window* windowPtr = nullptr;
    Client client([&windowPtr] {
        if (windowPtr != nullptr)
            windowPtr->requestRedrawFromAnyThread();
    });
    client.setAutoConnect(autoConnect);
    client.setSpectator(Tray::streamsToHeadset());

    // Panels on the left, the view on the right.
    SplitTree layout("eye");
    layout.dock("controls", "eye", Side::Left, 0.27);
    layout.dock("trace", "eye", Side::Right, 0.3);
    // A one-line status bar: the header plus a sparkline row, never shorter than both at 2x scale.
    layout.dock("stats", "eye", Side::Bottom, 0.07);
    layout.setMinSize("stats", Size{200, 2 * (24 + 30)});
    WindowConfig config;
    config.title = std::string("OXRSys XR Pilot ") + Version;
    config.width = 1600;
    config.height = 900;
    config.vulkanAllFeatures = true;
    config.tickHz = 90;
    config.lockLayout = true;
    config.aspectRatio = 16.0f / 9.0f; // OBS captures the window into a 16:9 canvas
    currentPalette = &palette(schemeFor(Appearance::Auto, systemDark()));
    config.theme = themeFrom(*currentPalette);
    std::string error;
    std::unique_ptr<Window> window = Window::create(config, layout, &error);
    if (!window)
    {
        shared.ui.record(UiAction::WindowError, false, error);
        reportError(agent, "The pilot window cannot open: " + error);
        return 1;
    }
    windowPtr = window.get();
    window->setPanel("eye", std::make_unique<EyePanel>(client, shared, *window));
    window->setPanel("controls", std::make_unique<ControlsPanel>(client, shared, &tray, window.get()));
    window->setPanel("stats", std::make_unique<StatsPanel>(client, shared, *window));
    window->setPanel("trace", std::make_unique<TracePanel>(shared));

    if (!client.start(&error))
    {
        shared.ui.record(UiAction::WindowError, false, error);
        reportError(agent, error);
        return 1;
    }
    shared.ui.record(UiAction::WindowOpen);

    const Answer answer = [&](const std::string& line) {
        SpanLine mark;
        const bool isMark = parseSpanLine(line, mark);
        if (isMark && mark.begin)
        {
            shared.spans.begin(mark.id, mark.name, mark.detail, nowNs() / 1'000'000);
            shared.traces.begin(mark.id, mark.name, mark.detail, nowNs() / 1'000'000);
        }
        else if (isMark)
        {
            shared.spans.end(mark.id, mark.ok, nowNs() / 1'000'000);
            shared.traces.end(mark.id, mark.ok, nowNs() / 1'000'000);
        }
        const ClientStatus status = client.status();
        CommandResult result;
        client.updateAgent([&](AgentState& s) { result = runCommand(line, s, status, shared.decoded.load()); });
        std::string reply = result.reply;
        if (!result.screenshotPath.empty())
        {
            std::unique_lock<std::mutex> lock(shared.mutex);
            shared.screenshotPath = result.screenshotPath;
            shared.screenshotReply.clear();
            windowPtr->requestRedrawFromAnyThread();
            if (!shared.done.wait_for(lock, std::chrono::seconds(5), [&] { return !shared.screenshotReply.empty(); }))
            {
                shared.screenshotPath.clear();
                shared.screenshotReply = errorJson("the window did not render within 5 s");
            }
            reply = shared.screenshotReply;
        }
        if (!isMark)
        {
            const bool ok = reply.rfind("{\"ok\":false", 0) != 0;
            shared.spans.child(line, ok, nowNs() / 1'000'000);
            shared.traces.command(line, ok, nowNs() / 1'000'000);
        }
        windowPtr->requestRedrawFromAnyThread();
        return reply;
    };
    if (bridge != nullptr)
        bridge->attach(answer);
    if (readCommands)
    {
        std::thread commands([&] {
            runCommandLoop(std::cin, std::cout, answer);
            // An agent that closes stdin is done, and the pilot exits with it; a person's window stays.
            if (agent)
            {
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            }
        });
        commands.detach();
    }

    const int rc = window->run();
    if (bridge != nullptr)
        bridge->detach();
    shared.ui.record(UiAction::WindowClose);
    client.stop();
    windowPtr = nullptr;
    window.reset();
    return rc;
}


// The MCP server's port on the loopback address; OXRSys's own ports are 9943 to 9948.
constexpr uint16_t McpPort = 9950;

// The install's MCP bearer token: made once, kept in the user's profile, which only they can read.
std::string mcpToken(const std::string& dir)
{
    const std::string path = dir + "mcp-token";
    if (SDL_IOStream* in = SDL_IOFromFile(path.c_str(), "rb"))
    {
        char buffer[129] = {};
        const size_t n = SDL_ReadIO(in, buffer, sizeof(buffer) - 1);
        SDL_CloseIO(in);
        std::string token(buffer, n);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
            token.pop_back();
        if (token.size() >= 32)
            return token;
    }
    std::random_device random;
    std::string token;
    for (int i = 0; i < 32; ++i)
    {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned>(random() & 0xFF));
        token += hex;
    }
    SDL_IOStream* out = SDL_IOFromFile(path.c_str(), "wb");
    if (out == nullptr)
        return {};
    SDL_WriteIO(out, token.data(), token.size());
    SDL_CloseIO(out);
#ifndef _WIN32
    chmod(path.c_str(), 0600);
#endif
    return token;
}

// xr_pilot_mcp --http --parent: its pilot commands reach the pilot window through the bridge.
class McpHost final
{
public:
    McpHost(CommandBridge& bridge, const std::string& token) : bridge_(bridge)
    {
        const char* base = SDL_GetBasePath();
        const std::string exe = std::string(base != nullptr ? base : "") + "xr_pilot_mcp" +
#ifdef _WIN32
                                ".exe";
#else
                                "";
#endif
        if (token.empty() || !std::filesystem::exists(exe))
            return;
        const std::string port = std::to_string(McpPort);
        const char* args[] = {exe.c_str(), "--http", port.c_str(), "--parent", nullptr};
        SDL_Environment* env = SDL_CreateEnvironment(true);
        SDL_SetEnvironmentVariable(env, "XR_PILOT_MCP_TOKEN", token.c_str(), true);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
        process_ = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        SDL_DestroyEnvironment(env);
        if (process_ == nullptr)
        {
            std::fprintf(stderr, "xr-pilot: the MCP server did not start: %s\n", SDL_GetError());
            return;
        }
        thread_ = std::thread([this] { run(); });
    }

    ~McpHost()
    {
        stop_ = true;
        if (process_ != nullptr)
            SDL_KillProcess(process_, true);
        if (thread_.joinable())
            thread_.join();
        if (process_ != nullptr)
            SDL_DestroyProcess(process_);
    }

    McpHost(const McpHost&) = delete;
    McpHost& operator=(const McpHost&) = delete;

private:
    // The pipes do not block: a read with nothing waiting returns 0 while the stream is NOT_READY.
    void run()
    {
        SDL_IOStream* out = SDL_GetProcessOutput(process_);
        SDL_IOStream* in = SDL_GetProcessInput(process_);
        std::string pending;
        char buffer[4096];
        while (!stop_)
        {
            const size_t n = SDL_ReadIO(out, buffer, sizeof(buffer));
            if (n == 0)
            {
                if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            pending.append(buffer, n);
            for (size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n'))
            {
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!normalizeCommandLine(line))
                    continue;
                const std::string reply = bridge_.ask(line, std::chrono::seconds(30)) + "\n";
                for (size_t sent = 0; sent < reply.size() && !stop_;)
                {
                    const size_t wrote = SDL_WriteIO(in, reply.data() + sent, reply.size() - sent);
                    sent += wrote;
                    if (wrote == 0 && SDL_GetIOStatus(in) != SDL_IO_STATUS_NOT_READY)
                        return;
                    if (wrote == 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                SDL_FlushIO(in);
            }
        }
    }

    CommandBridge& bridge_;
    SDL_Process* process_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace

namespace
{

UiAction launchAction(LaunchMode mode)
{
    switch (mode)
    {
    case LaunchMode::PilotThenTray:
        return UiAction::LaunchPilot;
    case LaunchMode::TrayOnly:
        return UiAction::LaunchTray;
    case LaunchMode::PilotOnly:
        return UiAction::LaunchWindow;
    case LaunchMode::Agent:
        return UiAction::LaunchAgent;
    case LaunchMode::ShowRunning:
        return UiAction::LaunchShowRunning;
    case LaunchMode::PrintVersion:
        return UiAction::LaunchVersion;
    case LaunchMode::Usage:
        break;
    }
    return UiAction::LaunchUsage;
}

} // namespace

int main(int argc, char** argv)
{
    const LaunchOptions launch = parseLaunch(argc, argv, Tray::deskCopyRunning());
    const bool agent = launch.mode == LaunchMode::Agent;

    // Traces open first, so every start, even one that only prints, is a span of the UI model.
    Shared shared;
    std::string tracePath = launch.tracePath;
    if (tracePath.empty())
    {
        char* pref = SDL_GetPrefPath("V-Sekai-fire", "xr-pilot");
        tracePath = std::string(pref != nullptr ? pref : "") + "traces.sqlite";
        SDL_free(pref);
    }
    {
        std::string traceError;
        const int64_t wallMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (!shared.traces.open(tracePath, wallMs, nowNs() / 1'000'000, &traceError))
            std::fprintf(stderr, "xr-pilot: traces not kept: %s\n", traceError.c_str());
        else if (launch.mode != LaunchMode::PrintVersion)
            std::fprintf(stderr, "xr-pilot: traces in %s, session %lld\n", tracePath.c_str(),
                         (long long)shared.traces.session());
    }

    switch (launch.mode)
    {
    case LaunchMode::PrintVersion:
        shared.ui.record(UiAction::LaunchVersion);
        std::printf("xr-pilot %s\n", Version);
        return 0;
    case LaunchMode::Usage:
        shared.ui.record(UiAction::LaunchUsage, false);
        std::fprintf(stderr,
                     "usage: xr-pilot [--version] [--tray | --window | --agent] [--snapshot out.png] [--record-stream out.pwrec] [--no-autoconnect] [--traces db.sqlite]\n"
                     "With no flags it opens the pilot and keeps the OXRSys tray after the pilot closes; --tray starts the tray alone.\n");
        return 2;
    case LaunchMode::ShowRunning:
    {
        // One desk copy: a second start brings the running pilot forward instead of binding the ports again.
        const bool asked = Tray::askRunningToShowPilot();
        shared.ui.record(UiAction::LaunchShowRunning, asked);
        if (asked)
            return 0;
        // A copy from before this release holds the tray but cannot be asked; say so rather than exit unseen.
        reportError(false, "XR Pilot is already running in the tray. Quit it from its tray icon, then start XR Pilot again.");
        return 1;
    }
    default:
        break;
    }
    shared.ui.record(launchAction(launch.mode));

    shared.snapshotPath = launch.snapshotPath;
    if (!launch.recordStreamPath.empty() && !shared.stream.open(launch.recordStreamPath))
    {
        reportError(agent, "cannot write " + launch.recordStreamPath);
        return 2;
    }
    const bool autoConnect = launch.autoConnect;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        reportError(agent, std::string("no display: ") + SDL_GetError());
        return 1;
    }
    // Only the desk copy sets up the desk and shows the tray; agent and scripted pilots have neither.
    Tray tray(ownsDesk(launch.mode), &shared.ui);
    if (ownsDesk(launch.mode) && !tray.ok())
        std::fprintf(stderr, "xr-pilot: no tray icon: %s\n", SDL_GetError());

    int rc = 0;
    if (launch.mode == LaunchMode::PilotOnly || agent)
        rc = runPilot(shared, autoConnect, agent, tray, true);
    else if (!tray.ok())
    {
        // With no tray icon, nothing could show the pilot again or quit: the pilot is the whole app,
        // and closing it quits.
        rc = runPilot(shared, autoConnect, false, tray, false);
    }
    else
    {
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
        bool quitting = false;
        SDL_AddEventWatch(
            [](void* flag, SDL_Event* e) {
                if (e->type == SDL_EVENT_QUIT)
                    *static_cast<bool*>(flag) = true;
                return true;
            },
            &quitting);
        CommandBridge bridge([] {
            SDL_Event show{};
            show.type = Tray::showPilotEvent();
            SDL_PushEvent(&show);
        });
        char* pref = SDL_GetPrefPath("V-Sekai-fire", "xr-pilot");
        McpHost mcp(bridge, mcpToken(pref != nullptr ? pref : ""));
        SDL_free(pref);
        // A first start opens the pilot; closing it leaves the tray, which can show it again.
        if (launch.mode == LaunchMode::PilotThenTray)
            rc = runPilot(shared, autoConnect, false, tray, false, &bridge);
        while (!quitting)
        {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 500) && e.type == Tray::showPilotEvent() && !bridge.attached())
                rc = runPilot(shared, autoConnect, false, tray, false, &bridge);
            tray.poll();
        }
    }
    shared.traces.close();
    if (!shared.stream.close())
        std::fprintf(stderr, "xr-pilot: the stream recording did not close cleanly\n");
    else if (shared.stream.frames() > 0)
        std::fprintf(stderr, "xr-pilot: recorded %llu streamed frames\n", (unsigned long long)shared.stream.frames());
    SDL_Quit();
    std::fflush(stdout);
    std::_Exit(rc);
}
