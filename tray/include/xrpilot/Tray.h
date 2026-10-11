// SPDX-License-Identifier: MPL-2.0
//
// The OXRSys tray, ported from OXRSys clients/Qt/oxrsys-home/src/HomeTray.{h,cpp} onto SDL3's tray:
// stream status, the default OpenXR runtime, unbinding, logs and the installed folders.

#pragma once

#include <SDL3/SDL.h>

#include "xrpilot/UiSpans.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xrpilot
{
class UiTrace;
}

class Tray final
{
public:
    // The desk copy (setUpDesk) installs the packaged OXRSys files, registers the SteamVR driver and
    // shows the tray icon; any other start has no tray. Each action is recorded through ui.
    explicit Tray(bool setUpDesk = true, xrpilot::UiTrace* ui = nullptr);
    ~Tray();

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    bool ok() const { return tray_ != nullptr; }
    // Follows the runtime's status file and finished background actions; call every couple of seconds.
    void poll();
    static Uint32 showPilotEvent();
    // Whether a desk copy (not an agent's pilot) already holds the tray.
    static bool deskCopyRunning();
    // Asks that copy to show its pilot; it does within its next poll.
    static bool askRunningToShowPilot();
    // Shows the pilot: raises its window, or asks the tray loop to open one.
    static void showPilot();
    // The tray's mode is Stream to a headset: a headset owns this machine's session, and any pilot here
    // watches as a spectator, lending its controllers to the hands the headset leaves free.
    static bool streamsToHeadset();

private:
    struct Choice
    {
        Tray* tray;
        std::string manifest;
    };

    void refresh();
    void rebuildRuntimeMenu();
    void rebuildDeveloperEntries();
    void makeDefaultRuntime(const std::string& manifest);
    void bind();
    void bindNow(const std::string& unused);
    void unbind();
    // SteamVR on OXRSys, with the desk's pilot owning the session ("steamvr") or watching it ("stream").
    void useSteamVrModeNow(const std::string& mode);
    void refreshModeEntries();
    void uninstall();
    void addRuntimeManifest();
    void setMessage(const std::string& message);
    void runInBackground(void (Tray::*action)(const std::string&), const std::string& argument);
    void makeDefaultRuntimeNow(const std::string& manifest);
    void unbindNow(const std::string& unused);
    // Unbinds; record is false when the uninstall does it on its way out.
    void unbindQuietly(bool record);
    void record(xrpilot::UiAction action, bool ok = true, const std::string& detail = std::string());

    static void onRuntime(void* userdata, SDL_TrayEntry* entry);
    static void onCommand(void* userdata, SDL_TrayEntry* entry);
    static void onFileChosen(void* userdata, const char* const* files, int filter);

    SDL_Tray* tray_ = nullptr;
    SDL_TrayMenu* menu_ = nullptr;
    SDL_TrayEntry* status_ = nullptr;
    SDL_TrayEntry* message_ = nullptr;
    SDL_TrayMenu* runtimeMenu_ = nullptr;
    SDL_TrayEntry* modeEntries_[3] = {};
    SDL_TrayEntry* developerMode_ = nullptr;
    std::vector<SDL_TrayEntry*> developerEntries_;
    SDL_Surface* idleIcon_ = nullptr;
    SDL_Surface* streamingIcon_ = nullptr;
    bool streaming_ = false;
    std::vector<std::string> shownRuntimes_;
    std::string shownActive_;
    std::vector<std::unique_ptr<Choice>> choices_;

    std::mutex mutex_;
    std::string messageText_;
    bool messageChanged_ = false;
    bool busy_ = false;
    void* instance_ = nullptr;
    void* showRequest_ = nullptr;
    xrpilot::UiTrace* ui_ = nullptr;
};
