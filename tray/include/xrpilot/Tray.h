// SPDX-License-Identifier: MPL-2.0
//
// The OXRSys tray, ported from OXRSys clients/Qt/oxrsys-home/src/HomeTray.{h,cpp} onto SDL3's tray:
// stream status, the default OpenXR runtime, unbinding, logs and the installed folders.

#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Tray final
{
public:
    Tray();
    ~Tray();

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    bool ok() const { return tray_ != nullptr; }
    // Follows the runtime's status file and finished background actions; call every couple of seconds.
    void poll();

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
    void uninstall();
    void addRuntimeManifest();
    void setMessage(const std::string& message);
    void runInBackground(void (Tray::*action)(const std::string&), const std::string& argument);
    void makeDefaultRuntimeNow(const std::string& manifest);
    void unbindNow(const std::string& unused);

    static void onRuntime(void* userdata, SDL_TrayEntry* entry);
    static void onCommand(void* userdata, SDL_TrayEntry* entry);
    static void onFileChosen(void* userdata, const char* const* files, int filter);

    SDL_Tray* tray_ = nullptr;
    SDL_TrayMenu* menu_ = nullptr;
    SDL_TrayEntry* status_ = nullptr;
    SDL_TrayEntry* message_ = nullptr;
    SDL_TrayMenu* runtimeMenu_ = nullptr;
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
};
