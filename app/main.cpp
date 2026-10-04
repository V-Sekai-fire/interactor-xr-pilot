// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// xr-pilot: an OXRSys client an agent drives. Commands arrive on stdin and replies leave on stdout,
// one per line (core/include/xrpilot/Commands.h); the window shows the decoded left eye and the
// agent's state. Decode, present and screenshots run on the window's thread and device.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "panelspun/widgets.h"
#include "panelspun/window.h"

#include "xrpilot/Client.h"
#include "xrpilot/Commands.h"
#include "xrpilot/GpuDecoder.h"
#include "xrpilot/Png.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace panelspun;
using namespace xrpilot;

namespace
{

struct Shared
{
    std::mutex mutex;
    std::condition_variable done;
    std::string screenshotPath; // pending request, served on the window thread
    std::string screenshotReply;
    std::atomic<uint64_t> decoded{0};
    std::atomic<uint64_t> decodeErrors{0};
    std::string lastCommand = "none";
};

class EyePanel final : public Panel
{
public:
    EyePanel(Client& client, Shared& shared)
        : Panel("Left eye")
        , client_(client)
        , shared_(shared)
    {
    }

    bool usesVulkanRegion() const override { return true; }
    void draw(DrawContext&) override {}

    void recordVulkan(const VulkanRegionFrame& frame) override
    {
        if (!ready_ && !failed_)
        {
            const VulkanContext& vk = *frame.context;
            GpuContext context{vk.getInstanceProcAddr, vk.instance, vk.physicalDevice, vk.device, vk.queue,
                               vk.queueFamily,         vk.instanceInfo, vk.deviceInfo};
            std::string error;
            ready_ = decoder_.initialize(context, &error);
            failed_ = !ready_;
            if (failed_)
                std::fprintf(stderr, "xr-pilot: %s\n", error.c_str());
        }
        if (ready_)
        {
            if (std::optional<AssembledVideoFrame> next = client_.takeFrame())
            {
                if (decoder_.decode(next->nalUnit.data(), next->nalUnit.size()))
                {
                    ++shared_.decoded;
                }
                else
                {
                    ++shared_.decodeErrors;
                    client_.requestKeyframe();
                }
            }
            serveScreenshot();
            decoder_.recordLeftEye(frame.commands, frame.image, frame.width, frame.height);
        }
    }

private:
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
    GpuDecoder decoder_;
    bool ready_ = false;
    bool failed_ = false;
};

class StatePanel final : public WidgetPanel
{
public:
    StatePanel(Client& client, Shared& shared)
        : WidgetPanel("Agent")
        , client_(client)
        , shared_(shared)
    {
        connection_ = add(std::make_unique<Label>("Searching for an OXRSys runtime"));
        stream_ = add(std::make_unique<Label>(""));
        head_ = add(std::make_unique<Label>(""));
        hands_ = add(std::make_unique<Label>(""));
        last_ = add(std::make_unique<Label>(""));
    }

    void draw(DrawContext& ctx) override
    {
        const ClientStatus s = client_.status();
        const AgentState a = client_.agent();
        connection_->setText(s.connected ? "Connected to " + s.serverName + " at " + s.server
                                         : "Searching for an OXRSys runtime");
        stream_->setText("Frames " + std::to_string(s.framesAssembled) + " received, " +
                         std::to_string(shared_.decoded.load()) + " decoded, " + std::to_string(s.framesDropped) +
                         " dropped");
        head_->setText("Head yaw " + std::to_string(int(a.head.yaw)) + "  pitch " + std::to_string(int(a.head.pitch)));
        hands_->setText(std::string("Right trigger ") + (a.hands[1].trigger > 0.5f ? "pressed" : "up") +
                        (a.hands[1].present ? "" : ", controllers absent"));
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            last_->setText("Last command: " + shared_.lastCommand);
        }
        WidgetPanel::draw(ctx);
    }

private:
    Client& client_;
    Shared& shared_;
    Label* connection_ = nullptr;
    Label* stream_ = nullptr;
    Label* head_ = nullptr;
    Label* hands_ = nullptr;
    Label* last_ = nullptr;
};

} // namespace

int main(int, char**)
{
    Shared shared;
    Window* windowPtr = nullptr;
    Client client([&windowPtr] {
        if (windowPtr != nullptr)
            windowPtr->requestRedrawFromAnyThread();
    });

    SplitTree layout("eye");
    layout.dock("state", "eye", Side::Right, 0.3);
    WindowConfig config;
    config.title = "OXRSys XR Pilot";
    config.width = 1400;
    config.height = 900;
    config.vulkanAllFeatures = true;
    std::string error;
    std::unique_ptr<Window> window = Window::create(config, layout, &error);
    if (!window)
    {
        std::fprintf(stderr, "xr-pilot: %s\n", error.c_str());
        return 1;
    }
    windowPtr = window.get();
    window->setPanel("eye", std::make_unique<EyePanel>(client, shared));
    window->setPanel("state", std::make_unique<StatePanel>(client, shared));

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
            AgentState state = client.agent();
            CommandResult result = runCommand(line, state, client.status(), shared.decoded.load());
            client.setAgent(state);
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
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                shared.lastCommand = line;
            }
            std::cout << reply << std::endl;
            windowPtr->requestRedrawFromAnyThread();
        }
        // The agent closed stdin: the pilot exits with it.
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    });
    commands.detach();

    const int rc = window->run();
    client.stop();
    windowPtr = nullptr;
    window.reset();
    SDL_Quit();
    std::fflush(stdout);
    std::_Exit(rc);
}
