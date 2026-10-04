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
#include "xrpilot/Commands.h"
#include "xrpilot/GpuDecoder.h"
#include "xrpilot/HumanInput.h"
#include "xrpilot/SpanLog.h"
#include "xrpilot/Png.h"
#include "xrpilot/Sparkline.h"
#include "xrpilot/Tray.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

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
    std::string snapshotPath; // --snapshot: the 90th decoded frame
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
                press(middleKey());
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
                release(key::M);
                release(key::HeadsetButton);
            }
        }
        else if (e.action == PointerAction::Move && (captured || dragging_))
        {
            mouseDx_ += e.dx;
            mouseDy_ += e.dy;
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
            const int64_t start = nowNs();
            if (decoder_.decode(next->nalUnit.data(), next->nalUnit.size()))
            {
                client_.reportLatency(*next, start, nowNs());
                consecutiveErrors_ = 0;
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
    }

    void recordVulkan(const VulkanRegionFrame& frame) override
    {
        if (ready_)
            decoder_.recordLeftEye(frame.commands, frame.image, frame.width, frame.height, false);
    }

private:
    int middleKey()
    {
        // With controllers the middle button is the menu; without them it is the headset button.
        return client_.agent().hands[1].present ? key::M : key::HeadsetButton;
    }

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
            keys_.erase(key::TriggerMouse);
    }

    void applyHumanInput()
    {
        const int64_t now = nowNs();
        const float dt = lastTickNs_ == 0 ? 0.0f : std::clamp(float(now - lastTickNs_) / 1e9f, 0.0f, 0.05f);
        lastTickNs_ = now;
        const std::set<int> keys = keys_;
        const float dx = mouseDx_, dy = mouseDy_, wheel = wheel_;
        mouseDx_ = mouseDy_ = wheel_ = 0.0f;
        client_.updateAgent([&](AgentState& s) {
            s.keys = keys;
            advanceHuman(s, dx, dy, dt);
            if (wheel != 0.0f)
            {
                // A wheel notch walks a quarter metre along where the head faces.
                float yawDegrees, pitchDegrees, rollDegrees;
                toYawPitchRoll(s.head.rotation, yawDegrees, pitchDegrees, rollDegrees);
                const float yaw = yawDegrees * 0.017453292f;
                s.head.position[0] += -std::sin(yaw) * 0.25f * wheel;
                s.head.position[2] += -std::cos(yaw) * 0.25f * wheel;
            }
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
    bool ready_ = false;
    bool failed_ = false;
    std::set<int> keys_;
    std::set<int> pressedThisTick_;
    std::set<int> releaseNextTick_;
    float mouseDx_ = 0.0f;
    float mouseDy_ = 0.0f;
    float wheel_ = 0.0f;
    bool dragging_ = false;
    float dragDistance_ = 0.0f;
    int64_t lastTickNs_ = 0;
    uint64_t lastDropped_ = 0;
    int consecutiveErrors_ = 0;
};

// One labelled sparkline row.
class SparkRow final : public Widget
{
public:
    SparkRow(std::string label, bool faultSeries)
        : label_(std::move(label))
        , fault_(faultSeries)
    {
    }

    void sample(uint64_t total) { line_.sample(total); }
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
        drawPolyline(ctx, base, 2, Color{65, 78, 94, 255}, 1.0f);
        for (const Sparkline::Segment& s : line_.segments(width, h - 2.0f, fault_))
        {
            const Color c = s.colour == Sparkline::Colour::Fault     ? Color{240, 98, 98, 255}
                            : s.colour == Sparkline::Colour::Clipped ? Color{242, 204, 96, 255}
                                                                     : Color{126, 231, 135, 255};
            const float xy[4] = {left + s.x0, y + 1.0f + s.y0, left + s.x1, y + 1.0f + s.y1};
            drawPolyline(ctx, xy, 2, c, 1.25f * ctx.scale);
        }
    }

private:
    std::string label_;
    bool fault_;
    Sparkline line_;
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
        const std::vector<Span> spans = shared_.spans.recent();
        const int64_t now = nowNs() / 1'000'000;
        const float s = ctx.scale;
        const float pad = 10.0f * s;
        const float row = 22.0f * s;
        const float sub = 17.0f * s;
        const float nameX = pad + 16.0f * s;
        const float barX = ctx.width * 0.48f;
        const float barW = std::max(10.0f, ctx.width - barX - 64.0f * s);
        float y = ctx.height - pad;
        int age = 0;
        for (std::vector<Span>::const_reverse_iterator it = spans.rbegin(); it != spans.rend() && y > 0.0f; ++it, ++age)
        {
            if (age == 0)
            {
                const size_t shown = std::min<size_t>(it->children.size(), 4);
                for (size_t c = it->children.size() - shown; c < it->children.size(); ++c)
                {
                    y -= sub;
                    const SpanChild& child = it->children[c];
                    std::string text = child.text;
                    if (text.size() > 40)
                        text = text.substr(0, 39) + "...";
                    tinted(ctx, child.ok ? Color{150, 156, 166, 255} : Red, [&](DrawContext& t) {
                        Label(text, 11.0f).draw(t, nameX + 10.0f * s, y, 0.0f);
                    });
                }
            }
            y -= row;
            const float fade = std::max(0.4f, 1.0f - 0.05f * float(age));
            const Color status = shade(it->running() ? Cyan : it->ok ? Green : Red, fade);
            const float mid = y + row * 0.5f;
            const float dot[4] = {pad + 4.0f * s, mid, pad + 4.0f * s + 0.1f, mid};
            drawPolyline(ctx, dot, 2, status, 8.0f * s);
            std::string name = it->name + (it->count > 1 ? "  x" + std::to_string(it->count) : "");
            tinted(ctx, shade(Color{223, 225, 229, 255}, fade),
                   [&](DrawContext& t) { Label(name, 13.0f).draw(t, nameX, y, 0.0f); });
            const int64_t ms = it->durationMs(now);
            const float span = std::min(1.0f, std::log10(1.0f + float(ms)) / std::log10(1.0f + 5000.0f));
            const float track[4] = {barX, mid, barX + barW, mid};
            drawPolyline(ctx, track, 2, shade(Color{55, 60, 68, 255}, fade), 6.0f * s);
            const float bar[4] = {barX, mid, barX + std::max(2.0f, barW * span), mid};
            drawPolyline(ctx, bar, 2, status, 6.0f * s);
            tinted(ctx, shade(Color{150, 156, 166, 255}, fade), [&](DrawContext& t) {
                Label(std::to_string(ms) + " ms", 11.0f).draw(t, barX + barW + 8.0f * s, y, 0.0f);
            });
        }
    }

private:
    static constexpr Color Cyan{98, 214, 255, 255};
    static constexpr Color Green{126, 231, 135, 255};
    static constexpr Color Red{240, 98, 98, 255};

    static Color shade(Color c, float f) { return Color{uint8_t(c.r * f), uint8_t(c.g * f), uint8_t(c.b * f), c.a}; }
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

// The stream's counters as a status bar of sparklines side by side, sampled every 250 ms: the last 10 s.
class StatsPanel final : public Panel
{
public:
    StatsPanel(Client& client, Shared& shared)
        : Panel("Stream")
        , client_(client)
        , shared_(shared)
        , rows_{SparkRow("pkt", false), SparkRow("fps", false), SparkRow("drop", true), SparkRow("fec", true),
                SparkRow("err", true)}
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
        for (int i = 0; i < 5; ++i)
            rows_[i].draw(ctx, pad + float(i) * (width + gap), y, width);
    }

    void update(const VulkanContext&) override
    {
        const int64_t now = nowNs();
        if (now - lastSampleNs_ < 250'000'000)
            return;
        lastSampleNs_ = now;
        const ClientStatus s = client_.status();
        const uint64_t totals[5] = {s.videoPackets, shared_.decoded.load(), s.framesDropped, s.fecRecoveries,
                                    shared_.decodeErrors.load()};
        for (int i = 0; i < 5; ++i)
            rows_[i].sample(totals[i]);
    }

private:
    Client& client_;
    Shared& shared_;
    SparkRow rows_[5];
    int64_t lastSampleNs_ = 0;
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
    ControlsPanel(Client& client, Shared& shared, Tray* tray)
        : WidgetPanel("XR Pilot")
        , tray_(tray)
        , client_(client)
        , shared_(shared)
    {
        connection_ = add(std::make_unique<Label>("Searching for an OXRSys runtime"));
        runtime_ = add(std::make_unique<Label>(""));
        add(std::make_unique<Button>("Connect", [this] { client_.connectNow(); }));
        add(std::make_unique<Button>("Disconnect", [this] { client_.disconnect(); }));
        autoConnect_ = add(std::make_unique<Toggle>("Connect automatically", true,
                                                    [this](bool on) { client_.setAutoConnect(on); }));
        fovLabel_ = add(std::make_unique<Label>("Vertical FOV 100 deg"));
        add(std::make_unique<Slider>(60.0f, 150.0f, 100.0f, [this](float v) {
            client_.updateAgent([v](AgentState& s) { s.verticalFovDegrees = v; });
        }));
        controllers_ = add(std::make_unique<Toggle>("Controllers", true, [this](bool on) {
            client_.updateAgent([on](AgentState& s) {
                s.hands[0].present = on;
                s.hands[1].present = on;
            });
        }));
        pointing_ = add(std::make_unique<Toggle>("Pointing (P)", false, [this](bool on) {
            client_.updateAgent([on](AgentState& s) {
                s.pointing = on;
                s.pointingAge = 0.0f;
            });
        }));
        seated_ = add(std::make_unique<Toggle>("Seated", false, [this](bool on) {
            client_.updateAgent([on](AgentState& s) { setSeated(s, on); });
        }));
        pose_ = add(std::make_unique<Label>(""));
        capture_ = add(std::make_unique<Label>(""));
        for (const char* help : {"Drag the view to look; click to capture, Esc lets go",
                                 "WASD walk, E/R roll, Shift moves a hand, wheel steps",
                                 "T/H/click triggers, F/G grips, 1-4 XYAB, M menu, P lowers"})
            add(std::make_unique<Label>(help, 11.0f));
    }

    void update(const VulkanContext&) override
    {
        const int64_t now = nowNs();
        if (tray_ != nullptr && now - lastTrayNs_ >= 2'000'000'000)
        {
            lastTrayNs_ = now;
            tray_->poll();
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
        pose_->setText("Head yaw " + fixed(yaw, 0) + "  pitch " + fixed(pitch, 0) + "  at " +
                       fixed(a.head.position[0], 2) + ", " + fixed(a.head.position[1], 2) + ", " +
                       fixed(a.head.position[2], 2));
        capture_->setText(shared_.captured ? "Mouse captured" : "Mouse free");
        WidgetPanel::draw(ctx);
    }

private:
    Client& client_;
    Shared& shared_;
    Label* connection_ = nullptr;
    Label* runtime_ = nullptr;
    Toggle* autoConnect_ = nullptr;
    Label* fovLabel_ = nullptr;
    Toggle* controllers_ = nullptr;
    Toggle* pointing_ = nullptr;
    Toggle* seated_ = nullptr;
    Label* pose_ = nullptr;
    Label* capture_ = nullptr;
    Tray* tray_ = nullptr;
    int64_t lastTrayNs_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    Shared shared;
    bool autoConnect = true;
    bool agent = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--snapshot") && i + 1 < argc)
            shared.snapshotPath = argv[++i];
        else if (!std::strcmp(argv[i], "--no-autoconnect"))
            autoConnect = false;
        else if (!std::strcmp(argv[i], "--agent"))
            agent = true;
        else
        {
            std::fprintf(stderr, "usage: xr-pilot [--agent] [--snapshot out.png] [--no-autoconnect]\n");
            return 2;
        }
    }

    Window* windowPtr = nullptr;
    Client client([&windowPtr] {
        if (windowPtr != nullptr)
            windowPtr->requestRedrawFromAnyThread();
    });
    client.setAutoConnect(autoConnect);

    // Panels on the left, the view on the right.
    SplitTree layout("eye");
    layout.dock("controls", "eye", Side::Left, 0.27);
    layout.dock("trace", "eye", Side::Right, 0.26);
    // A one-line status bar: the header plus a sparkline row, never shorter than both at 2x scale.
    layout.dock("stats", "eye", Side::Bottom, 0.07);
    layout.setMinSize("stats", Size{200, 2 * (24 + 30)});
    WindowConfig config;
    config.title = "OXRSys XR Pilot";
    config.width = 1400;
    config.height = 900;
    config.vulkanAllFeatures = true;
    config.tickHz = 90;
    config.lockLayout = true;
    std::string error;
    std::unique_ptr<Window> window = Window::create(config, layout, &error);
    if (!window)
    {
        std::fprintf(stderr, "xr-pilot: %s\n", error.c_str());
        return 1;
    }
    windowPtr = window.get();
    window->setPanel("eye", std::make_unique<EyePanel>(client, shared, *window));
    // Every pilot carries the OXRSys tray; one an agent starts leaves the desk's install alone at start.
    std::unique_ptr<Tray> tray = std::make_unique<Tray>(!agent);
    if (!tray->ok())
        std::fprintf(stderr, "xr-pilot: no tray icon: %s\n", SDL_GetError());
    window->setPanel("controls", std::make_unique<ControlsPanel>(client, shared, tray.get()));
    window->setPanel("stats", std::make_unique<StatsPanel>(client, shared));
    window->setPanel("trace", std::make_unique<TracePanel>(shared));

    if (!client.start(&error))
    {
        std::fprintf(stderr, "xr-pilot: %s\n", error.c_str());
        return 1;
    }

    std::thread commands([&] {
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            // Some shells start the stream with a UTF-8 byte order mark.
            if (line.rfind("\xEF\xBB\xBF", 0) == 0)
                line.erase(0, 3);
            if (line.empty())
                continue;
            SpanLine mark;
            const bool isMark = parseSpanLine(line, mark);
            if (isMark && mark.begin)
                shared.spans.begin(mark.id, mark.name, mark.detail, nowNs() / 1'000'000);
            else if (isMark)
                shared.spans.end(mark.id, mark.ok, nowNs() / 1'000'000);
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
                shared.spans.child(line, reply.rfind("{\"ok\":false", 0) != 0, nowNs() / 1'000'000);
            std::cout << reply << std::endl;
            windowPtr->requestRedrawFromAnyThread();
        }
        // An agent that closes stdin is done, and the pilot exits with it; a person's window stays.
        if (agent)
        {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
    });
    commands.detach();

    const int rc = window->run();
    client.stop();
    windowPtr = nullptr;
    window.reset();
    tray.reset();
    SDL_Quit();
    std::fflush(stdout);
    std::_Exit(rc);
}
