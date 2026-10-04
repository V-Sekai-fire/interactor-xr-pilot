// SPDX-License-Identifier: MPL-2.0

#include "xrpilot/Tray.h"

#include "xrpilot/Install.h"
#include "xrpilot/Json.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>
#endif

using xrpilot::Json;

namespace
{

namespace fs = std::filesystem;

std::string readFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

Json readJson(const fs::path& path)
{
    Json j;
    if (!xrpilot::parseJson(readFile(path), j))
        j = Json();
    return j;
}

bool sameText(const std::string& a, const std::string& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

std::string native(std::string path)
{
#if defined(_WIN32)
    std::replace(path.begin(), path.end(), '/', '\\');
#endif
    return path;
}

#if defined(_WIN32)

std::wstring wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string environment(const char* name)
{
    const DWORD n = GetEnvironmentVariableW(wide(name).c_str(), nullptr, 0);
    if (n == 0)
        return {};
    std::wstring value(n, L'\0');
    GetEnvironmentVariableW(wide(name).c_str(), value.data(), n);
    value.resize(n - 1);
    return narrow(value);
}

// The running package's full name, empty outside a package.
std::string packageFullName()
{
    wchar_t name[PACKAGE_FULL_NAME_MAX_LENGTH + 1] = {};
    UINT32 length = PACKAGE_FULL_NAME_MAX_LENGTH + 1;
    return GetCurrentPackageFullName(&length, name) == ERROR_SUCCESS ? narrow(name) : std::string();
}

// A package copies the runtime and driver into its own LocalCache, a real folder other processes can
// load from that Windows deletes with the package; a development build uses windows_build.ps1 -Install's folder.
std::string installBase()
{
    wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1] = {};
    UINT32 length = PACKAGE_FAMILY_NAME_MAX_LENGTH + 1;
    if (GetCurrentPackageFamilyName(&length, family) == ERROR_SUCCESS)
        return environment("LOCALAPPDATA") + "\\Packages\\" + narrow(family) + "\\LocalCache\\OXRSys";
    return environment("LOCALAPPDATA") + "\\OXRSys";
}

// The Qt tray's settings, kept where it wrote them so its choices carry over.
constexpr const wchar_t* SettingsKey = L"Software\\OXRSys\\HomeQt\\tray";

bool settingFlag(const wchar_t* name)
{
    wchar_t value[16] = {};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, SettingsKey, name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
           std::wstring(value) == L"true";
}

void setSettingFlag(const wchar_t* name, bool on)
{
    const wchar_t* text = on ? L"true" : L"false";
    RegSetKeyValueW(HKEY_CURRENT_USER, SettingsKey, name, REG_SZ, text, DWORD((wcslen(text) + 1) * sizeof(wchar_t)));
}

std::vector<std::string> settingList(const wchar_t* name)
{
    std::vector<std::string> out;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, SettingsKey, name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return out;
    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, SettingsKey, name, RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS)
        return out;
    for (const wchar_t* p = buffer.c_str(); *p != L'\0'; p += wcslen(p) + 1)
        out.push_back(narrow(p));
    return out;
}

void setSettingList(const wchar_t* name, const std::vector<std::string>& values)
{
    std::wstring block;
    for (const std::string& v : values)
    {
        block += wide(v);
        block += L'\0';
    }
    block += L'\0';
    RegSetKeyValueW(HKEY_CURRENT_USER, SettingsKey, name, REG_MULTI_SZ, block.data(), DWORD(block.size() * sizeof(wchar_t)));
}

std::string openXrValue(const wchar_t* name)
{
    wchar_t value[2048] = {};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", name, RRF_RT_REG_SZ, nullptr, value, &size) ==
                   ERROR_SUCCESS
               ? narrow(value)
               : std::string();
}

std::vector<std::string> availableRuntimes()
{
    std::vector<std::string> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return names;
    for (DWORD index = 0;; ++index)
    {
        wchar_t name[1024] = {};
        DWORD length = 1024;
        if (RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        names.push_back(narrow(std::wstring(name, length)));
    }
    RegCloseKey(key);
    return names;
}

bool processRunning(int64_t pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (process == nullptr)
        return false;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

void reveal(const std::string& folder)
{
    fs::create_directories(fs::path(wide(folder)));
    ShellExecuteW(nullptr, L"open", wide(folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int runHidden(const std::wstring& exe, const std::wstring& arguments, DWORD timeoutMs)
{
    std::wstring line = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        return -1;
    WaitForSingleObject(info.hProcess, timeoutMs);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return int(code);
}

std::string base64(const std::string& bytes)
{
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const uint32_t b0 = uint8_t(bytes[i]);
        const uint32_t b1 = i + 1 < bytes.size() ? uint8_t(bytes[i + 1]) : 0;
        const uint32_t b2 = i + 2 < bytes.size() ? uint8_t(bytes[i + 2]) : 0;
        const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? table[n & 63] : '=';
    }
    return out;
}

// HKLM needs elevation: one UAC prompt runs the PowerShell, and this waits for it.
bool runElevatedPowerShell(const std::string& script)
{
    const std::wstring utf16 = wide(script);
    const std::string encoded = base64(std::string(reinterpret_cast<const char*>(utf16.data()), utf16.size() * 2));
    const std::wstring parameters = L"-NoProfile -WindowStyle Hidden -EncodedCommand " + wide(encoded);
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = L"powershell.exe";
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || info.hProcess == nullptr)
        return false;
    WaitForSingleObject(info.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    return exitCode == 0;
}

#else

std::string environment(const char* name)
{
    const char* v = SDL_getenv(name);
    return v ? v : "";
}
std::string packageFullName() { return {}; }
std::string installBase() { return environment("HOME") + "/.local/share/oxrsys"; }
bool settingFlag(const wchar_t*) { return false; }
void setSettingFlag(const wchar_t*, bool) {}
std::vector<std::string> settingList(const wchar_t*) { return {}; }
void setSettingList(const wchar_t*, const std::vector<std::string>&) {}
std::string openXrValue(const wchar_t*) { return {}; }
std::vector<std::string> availableRuntimes() { return {}; }
bool processRunning(int64_t) { return true; }
void reveal(const std::string& folder) { SDL_OpenURL(("file://" + folder).c_str()); }
bool runElevatedPowerShell(const std::string&) { return false; }

#endif

std::string installedRuntimeManifest()
{
    return native(installBase() + "/runtime/oxrsys-runtime.json");
}

std::string installedDriverFolder()
{
    return native(installBase() + "/driver/oxrsys");
}

std::string stateFolder()
{
    return native(environment("LOCALAPPDATA") + "/OXRSys");
}

// A package ships the runtime and driver beside the app, which sits in the package's bin folder.
fs::path packageRoot()
{
    const char* base = SDL_GetBasePath();
    return base != nullptr ? fs::path(base).parent_path().parent_path() : fs::path();
}

// An OXRSys build when the pilot is not packaged: the workspace's oxrsys checkout beside this one,
// as windows_build.ps1 leaves it.
fs::path oxrsysBuild()
{
    const char* base = SDL_GetBasePath();
    return base != nullptr ? fs::path(base).parent_path().parent_path().parent_path() / "oxrsys" / "build" / "windows"
                           : fs::path();
}

// Windows will not load a packaged DLL into another app's process, so a package's runtime and driver
// are copied out to installBase(); unpackaged, they come from the oxrsys build.
bool installRuntime(std::string* error)
{
    const fs::path source = xrpilot::isOxrsysSource(packageRoot()) ? packageRoot() : oxrsysBuild();
    return xrpilot::installOxrsys(source, fs::path(installBase()), error);
}

void installPackagedFiles()
{
    if (xrpilot::isOxrsysSource(packageRoot()))
        xrpilot::installOxrsys(packageRoot(), fs::path(installBase()), nullptr);
}

Json openVrPaths()
{
    return readJson(fs::path(environment("LOCALAPPDATA")) / "openvr" / "openvrpaths.vrpath");
}

std::string firstString(const Json* array)
{
    return array != nullptr && !array->items.empty() ? array->items[0].str() : std::string();
}

// The user's unbind is remembered, so starting again does not undo it.
bool bindingWanted()
{
    return !settingFlag(L"unbound");
}

void runVrPathReg(const std::string& verb, const std::string& folder)
{
#if defined(_WIN32)
    const std::string steamVr = firstString(openVrPaths().find("runtime"));
    if (!steamVr.empty())
        runHidden(wide(steamVr + "\\bin\\win64\\vrpathreg.exe"), wide(verb + " \"" + folder + "\""), 15000);
#else
    (void)verb, (void)folder;
#endif
}

std::string fileName(const std::string& path)
{
    return fs::path(path).filename().string();
}

// Registers the installed driver with SteamVR's own vrpathreg; per-user, so no prompt.
void registerSteamVrDriver()
{
    if (!bindingWanted() || !fs::exists(installedDriverFolder()))
        return;
    bool registered = false;
    const Json paths = openVrPaths();
    if (const Json* drivers = paths.find("external_drivers"))
    {
        for (const Json& d : drivers->items)
        {
            const std::string folder = native(d.str());
            if (sameText(folder, installedDriverFolder()))
                registered = true;
            else if (sameText(fileName(folder), "oxrsys"))
                runVrPathReg("removedriver", folder);
        }
    }
    if (!registered)
        runVrPathReg("adddriver", installedDriverFolder());
}

// Removes every registered folder of this driver, including ones a development build registered.
int unregisterSteamVrDriver()
{
    int removed = 0;
    const Json paths = openVrPaths();
    if (const Json* drivers = paths.find("external_drivers"))
    {
        for (const Json& d : drivers->items)
        {
            const std::string folder = native(d.str());
            if (sameText(fileName(folder), "oxrsys"))
            {
                runVrPathReg("removedriver", folder);
                ++removed;
            }
        }
    }
    return removed;
}

// SteamVR picks one headset driver among those that load; forcedDriver makes it ours while OXRSys is
// the picked runtime, and picking another runtime hands the choice back. Returns whether it changed.
bool useSteamVrHeadset(bool ours)
{
    const std::string config = firstString(openVrPaths().find("config"));
    if (config.empty())
        return false;
    const fs::path path = fs::path(config) / "steamvr.vrsettings";
    Json settings = readJson(path);
    if (settings.kind != Json::Kind::Object)
        return false;
    Json steamvr;
    if (const Json* existing = settings.find("steamvr"))
        steamvr = *existing;
    const bool forced = steamvr.find("forcedDriver") != nullptr && steamvr.find("forcedDriver")->str() == "oxrsys";
    if (ours == forced)
        return false;
    if (ours)
        steamvr.set("forcedDriver", Json::string("oxrsys"));
    else
        steamvr.erase("forcedDriver");
    settings.set("steamvr", steamvr);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << xrpilot::writeJson(settings);
    return bool(file);
}

std::string runtimeName(const std::string& manifest)
{
    const Json j = readJson(manifest);
    const Json* runtime = j.find("runtime");
    const std::string name = runtime != nullptr && runtime->find("name") != nullptr ? runtime->find("name")->str() : "";
    return name.empty() ? fs::path(manifest).stem().string() : name;
}

std::string quoted(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += c == '\'' ? std::string("''") : std::string(1, c);
    return out;
}

// The runtime's status file: streaming or idle, and to which device and app.
std::string activityText(bool& streaming)
{
    streaming = false;
    const Json status = readJson(fs::path(stateFolder()) / "runtime_status.json");
    if (status.kind != Json::Kind::Object)
        return "Idle";
    const Json* pid = status.find("process_id");
    if (pid != nullptr && pid->kind == Json::Kind::Number && !processRunning(std::atoll(pid->text.c_str())))
        return "Idle";
    const Json* state = status.find("state");
    if (state == nullptr || state->str() != "streaming")
        return "Idle";
    streaming = true;
    const Json* device = status.find("device_type");
    const Json* app = status.find("application_name");
    const std::string type = device != nullptr ? device->str() : "";
    const std::string deviceName = type == "quest"      ? "Quest"
                                   : type == "pico"      ? "Pico"
                                   : type == "simulator" ? "Simulator"
                                   : type.empty()        ? "Unknown"
                                                         : type;
    return "Streaming: " + deviceName + " -> " + (app != nullptr ? app->str() : std::string());
}

SDL_Surface* loadIcon(const char* name)
{
    const char* base = SDL_GetBasePath();
    return base != nullptr ? SDL_LoadPNG((std::string(base) + "resources/" + name).c_str()) : nullptr;
}

enum Command : intptr_t
{
    CommandBind = 1,
    CommandUnbind,
    CommandUninstall,
    CommandShowPilot,
    CommandLogs,
    CommandRuntimeFolder,
    CommandDriverFolder,
    CommandDeveloperMode,
    CommandAddRuntime,
    CommandQuit,
};

struct CommandTarget
{
    Tray* tray;
    Command command;
};

std::vector<std::unique_ptr<CommandTarget>>& commandTargets()
{
    static std::vector<std::unique_ptr<CommandTarget>> targets;
    return targets;
}

} // namespace

Tray::Tray(bool setUpDesk)
{
    if (setUpDesk)
    {
        installPackagedFiles();
        registerSteamVrDriver();
    }
    idleIcon_ = loadIcon("tray_idle.png");
    streamingIcon_ = loadIcon("tray_streaming.png");
    tray_ = SDL_CreateTray(idleIcon_, "OXRSys");
    if (tray_ == nullptr)
        return;
    menu_ = SDL_CreateTrayMenu(tray_);
    status_ = SDL_InsertTrayEntryAt(menu_, -1, "Idle", SDL_TRAYENTRY_BUTTON | SDL_TRAYENTRY_DISABLED);
    message_ = SDL_InsertTrayEntryAt(menu_, -1, "", SDL_TRAYENTRY_BUTTON | SDL_TRAYENTRY_DISABLED);
    SDL_InsertTrayEntryAt(menu_, -1, nullptr, 0);
    SDL_TrayEntry* runtimes = SDL_InsertTrayEntryAt(menu_, -1, "Default OpenXR runtime", SDL_TRAYENTRY_SUBMENU);
    runtimeMenu_ = SDL_CreateTraySubmenu(runtimes);

    const std::pair<const char*, Command> commands[] = {
        {"Bind OXRSys", CommandBind},
        {"Unbind OXRSys", CommandUnbind},
        {"Uninstall OXRSys...", CommandUninstall},
        {"Show XR Pilot", CommandShowPilot},
        {"Open logs", CommandLogs},
    };
    for (const std::pair<const char*, Command>& c : commands)
    {
        if (c.second == CommandUninstall && packageFullName().empty())
            continue;
        SDL_TrayEntry* entry = SDL_InsertTrayEntryAt(menu_, -1, c.first, SDL_TRAYENTRY_BUTTON);
        commandTargets().push_back(std::make_unique<CommandTarget>(CommandTarget{this, c.second}));
        SDL_SetTrayEntryCallback(entry, &Tray::onCommand, commandTargets().back().get());
    }
    SDL_InsertTrayEntryAt(menu_, -1, nullptr, 0);
    developerMode_ = SDL_InsertTrayEntryAt(menu_, -1, "Developer mode",
                                           SDL_TRAYENTRY_CHECKBOX | (settingFlag(L"developerMode") ? SDL_TRAYENTRY_CHECKED : 0u));
    commandTargets().push_back(std::make_unique<CommandTarget>(CommandTarget{this, CommandDeveloperMode}));
    SDL_SetTrayEntryCallback(developerMode_, &Tray::onCommand, commandTargets().back().get());
    SDL_TrayEntry* quit = SDL_InsertTrayEntryAt(menu_, -1, "Quit", SDL_TRAYENTRY_BUTTON);
    commandTargets().push_back(std::make_unique<CommandTarget>(CommandTarget{this, CommandQuit}));
    SDL_SetTrayEntryCallback(quit, &Tray::onCommand, commandTargets().back().get());
    rebuildDeveloperEntries();
    refresh();
}

Tray::~Tray()
{
    if (tray_ != nullptr)
        SDL_DestroyTray(tray_);
    SDL_DestroySurface(idleIcon_);
    SDL_DestroySurface(streamingIcon_);
}

void Tray::poll()
{
    if (tray_ != nullptr)
        refresh();
}

void Tray::refresh()
{
    bool streaming = false;
    const std::string status = activityText(streaming);
    SDL_SetTrayEntryLabel(status_, status.c_str());
    SDL_SetTrayTooltip(tray_, ("OXRSys: " + status).c_str());
    if (streaming != streaming_ && (streaming ? streamingIcon_ : idleIcon_) != nullptr)
        SDL_SetTrayIcon(tray_, streaming ? streamingIcon_ : idleIcon_);
    streaming_ = streaming;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (messageChanged_)
        {
            SDL_SetTrayEntryLabel(message_, messageText_.c_str());
            messageChanged_ = false;
        }
    }
    rebuildRuntimeMenu();
}

// Runtimes the desk has shown or the user added: the loader's list, the active and previous ones,
// ours, and every one remembered, since a vendor may set ActiveRuntime without listing itself.
void Tray::rebuildRuntimeMenu()
{
    std::vector<std::string> known = settingList(L"knownRuntimes");
    for (const std::string& r : availableRuntimes())
        known.push_back(r);
    known.push_back(openXrValue(L"ActiveRuntime"));
    known.push_back(openXrValue(L"PreviousActiveRuntime"));
    known.push_back(installedRuntimeManifest());
    std::vector<std::string> runtimes;
    for (const std::string& path : known)
    {
        const std::string manifest = native(path);
        const bool listed = std::any_of(runtimes.begin(), runtimes.end(),
                                        [&](const std::string& r) { return sameText(r, manifest); });
        std::error_code error;
        if (!manifest.empty() && !listed && fs::exists(fs::path(manifest), error))
            runtimes.push_back(manifest);
    }
    const std::string active = native(openXrValue(L"ActiveRuntime"));
    if (runtimes == shownRuntimes_ && sameText(active, shownActive_))
        return;
    setSettingList(L"knownRuntimes", runtimes);
    shownRuntimes_ = runtimes;
    shownActive_ = active;

    int count = 0;
    const SDL_TrayEntry** entries = SDL_GetTrayEntries(runtimeMenu_, &count);
    std::vector<SDL_TrayEntry*> old;
    for (int i = 0; i < count; ++i)
        old.push_back(const_cast<SDL_TrayEntry*>(entries[i]));
    for (SDL_TrayEntry* e : old)
        SDL_RemoveTrayEntry(e);
    choices_.clear();
    for (const std::string& manifest : runtimes)
    {
        const SDL_TrayEntryFlags checked = sameText(manifest, active) ? SDL_TRAYENTRY_CHECKED : 0u;
        SDL_TrayEntry* item = SDL_InsertTrayEntryAt(runtimeMenu_, -1, runtimeName(manifest).c_str(), SDL_TRAYENTRY_CHECKBOX | checked);
        choices_.push_back(std::make_unique<Choice>(Choice{this, manifest}));
        SDL_SetTrayEntryCallback(item, &Tray::onRuntime, choices_.back().get());
    }
    SDL_InsertTrayEntryAt(runtimeMenu_, -1, nullptr, 0);
    SDL_TrayEntry* add = SDL_InsertTrayEntryAt(runtimeMenu_, -1, "Add runtime manifest...", SDL_TRAYENTRY_BUTTON);
    commandTargets().push_back(std::make_unique<CommandTarget>(CommandTarget{this, CommandAddRuntime}));
    SDL_SetTrayEntryCallback(add, &Tray::onCommand, commandTargets().back().get());
}

void Tray::rebuildDeveloperEntries()
{
    for (SDL_TrayEntry* e : developerEntries_)
        SDL_RemoveTrayEntry(e);
    developerEntries_.clear();
    if (!settingFlag(L"developerMode"))
        return;
    int count = 0;
    const SDL_TrayEntry** entries = SDL_GetTrayEntries(menu_, &count);
    int at = 0;
    for (int i = 0; i < count; ++i)
    {
        if (entries[i] == developerMode_)
            at = i - 1;
    }
    const std::pair<const char*, Command> items[] = {
        {"Open installed runtime folder", CommandRuntimeFolder},
        {"Open PC VR driver folder", CommandDriverFolder},
    };
    for (const std::pair<const char*, Command>& item : items)
    {
        SDL_TrayEntry* entry = SDL_InsertTrayEntryAt(menu_, at++, item.first, SDL_TRAYENTRY_BUTTON);
        commandTargets().push_back(std::make_unique<CommandTarget>(CommandTarget{this, item.second}));
        SDL_SetTrayEntryCallback(entry, &Tray::onCommand, commandTargets().back().get());
        developerEntries_.push_back(entry);
    }
}

void Tray::setMessage(const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    messageText_ = message;
    messageChanged_ = true;
}

// Elevated changes wait up to a minute on a UAC prompt, so they run off the window's thread.
void Tray::runInBackground(void (Tray::*action)(const std::string&), const std::string& argument)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_)
            return;
        busy_ = true;
    }
    std::thread([this, action, argument] {
        (this->*action)(argument);
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
    }).detach();
}

void Tray::makeDefaultRuntime(const std::string& manifest)
{
    setMessage("Asking for administrator approval...");
    runInBackground(&Tray::makeDefaultRuntimeNow, manifest);
}

void Tray::makeDefaultRuntimeNow(const std::string& manifest)
{
    const bool ours = sameText(manifest, installedRuntimeManifest());
    if (ours)
    {
        setSettingFlag(L"unbound", false);
        registerSteamVrDriver();
    }
    const std::string key = "HKLM:\\SOFTWARE\\Khronos\\OpenXR\\1";
    const std::string active = openXrValue(L"ActiveRuntime");
    std::string script = "New-Item -Force -Path '" + key + "\\AvailableRuntimes' | Out-Null; Set-ItemProperty -Path '" + key +
                         "\\AvailableRuntimes' -Name '" + quoted(manifest) + "' -Value 0 -Type DWord; ";
    if (!active.empty() && !sameText(active, manifest))
        script += "Set-ItemProperty -Path '" + key + "' -Name PreviousActiveRuntime -Value '" + quoted(active) + "'; ";
    script += "Set-ItemProperty -Path '" + key + "' -Name ActiveRuntime -Value '" + quoted(manifest) + "'";
    if (!runElevatedPowerShell(script))
    {
        setMessage("Default runtime unchanged: the administrator prompt was declined");
        return;
    }
    const bool headsetChanged = useSteamVrHeadset(ours);
    setMessage("Default runtime: " + runtimeName(manifest) + (headsetChanged ? "; restart SteamVR for its headset to follow" : ""));
}

// One step: install the runtime and driver, register the driver with SteamVR, and make OXRSys the
// default OpenXR runtime and SteamVR's headset, behind one administrator prompt.
void Tray::bind()
{
    setMessage("Installing OXRSys...");
    runInBackground(&Tray::bindNow, std::string());
}

void Tray::bindNow(const std::string&)
{
    std::string error;
    if (!installRuntime(&error))
    {
        setMessage("Not installed: " + error);
        return;
    }
    shownRuntimes_.clear();
    makeDefaultRuntimeNow(installedRuntimeManifest());
}

void Tray::unbind()
{
    setMessage("Unbinding...");
    runInBackground(&Tray::unbindNow, std::string());
}

// Undoes everything binding did: the OpenXR default goes back to the previous runtime (or is cleared)
// and ours leaves the loader's list, the driver leaves SteamVR's list, and SteamVR's headset choice is
// handed back. Picking OXRSys in the runtime menu binds it again.
void Tray::unbindNow(const std::string&)
{
    setSettingFlag(L"unbound", true);
    const int drivers = unregisterSteamVrDriver();
    useSteamVrHeadset(false);
    std::string runtime = "left as it was";
    const std::string ours = installedRuntimeManifest();
    const std::string active = native(openXrValue(L"ActiveRuntime"));
    const std::string previous = native(openXrValue(L"PreviousActiveRuntime"));
    const std::vector<std::string> available = availableRuntimes();
    const bool listed = std::any_of(available.begin(), available.end(), [&](const std::string& r) { return sameText(r, ours); });
    const bool activeIsOurs = sameText(active, ours);
    const bool previousIsOurs = sameText(previous, ours);
    if (activeIsOurs || listed || previousIsOurs)
    {
        const std::string key = "HKLM:\\SOFTWARE\\Khronos\\OpenXR\\1";
        std::string script = "Remove-ItemProperty -Path '" + key + "\\AvailableRuntimes' -Name '" + quoted(ours) +
                             "' -ErrorAction SilentlyContinue; ";
        if (previousIsOurs)
            script += "Remove-ItemProperty -Path '" + key + "' -Name PreviousActiveRuntime -ErrorAction SilentlyContinue; ";
        std::error_code error;
        const bool restore = activeIsOurs && !previous.empty() && !previousIsOurs && fs::exists(fs::path(previous), error);
        if (restore)
            script += "Set-ItemProperty -Path '" + key + "' -Name ActiveRuntime -Value '" + quoted(previous) + "'";
        else if (activeIsOurs)
            script += "Remove-ItemProperty -Path '" + key + "' -Name ActiveRuntime -ErrorAction SilentlyContinue";
        if (runElevatedPowerShell(script))
            runtime = restore ? "back to " + runtimeName(previous) : activeIsOurs ? "cleared" : "left as it was";
        else
            runtime = "unchanged: the administrator prompt was declined";
    }
    setMessage("Unbound: default runtime " + runtime + "; " + std::to_string(drivers) +
               " SteamVR driver registration(s) removed; restart SteamVR");
}

// A package cannot run code when Windows removes it, so the tray unbinds first and then removes its
// own package; the copied runtime and driver go with the package's LocalCache.
void Tray::uninstall()
{
    const std::string package = packageFullName();
    if (package.empty())
        return;
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Uninstall"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_WARNING, nullptr, "Uninstall OXRSys",
                                    "Unbind OXRSys from OpenXR and SteamVR, then remove it?", 2, buttons, nullptr};
    int choice = 0;
    if (!SDL_ShowMessageBox(&box, &choice) || choice != 1)
        return;
    unbindNow(std::string());
#if defined(_WIN32)
    std::error_code error;
    fs::remove_all(fs::path(wide(environment("LOCALAPPDATA") + "\\OXRSys")), error);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\OXRSys\\HomeQt");
    const std::wstring script = L"Start-Sleep -Seconds 2; Remove-AppxPackage -Package '" + wide(package) + L"'";
    ShellExecuteW(nullptr, L"open", L"powershell.exe",
                  (L"-NoProfile -WindowStyle Hidden -Command \"" + script + L"\"").c_str(), nullptr, SW_HIDE);
#endif
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

void Tray::addRuntimeManifest()
{
    static const SDL_DialogFileFilter filter = {"Runtime manifest", "json"};
    SDL_ShowOpenFileDialog(&Tray::onFileChosen, this, nullptr, &filter, 1, nullptr, false);
}

void Tray::onFileChosen(void* userdata, const char* const* files, int)
{
    if (files == nullptr || files[0] == nullptr)
        return;
    Tray* self = static_cast<Tray*>(userdata);
    std::vector<std::string> known = settingList(L"knownRuntimes");
    known.push_back(native(files[0]));
    setSettingList(L"knownRuntimes", known);
    self->shownRuntimes_.clear();
}

void Tray::onRuntime(void* userdata, SDL_TrayEntry*)
{
    const Choice* choice = static_cast<const Choice*>(userdata);
    choice->tray->makeDefaultRuntime(choice->manifest);
}

void Tray::onCommand(void* userdata, SDL_TrayEntry*)
{
    const CommandTarget* target = static_cast<const CommandTarget*>(userdata);
    Tray* self = target->tray;
    switch (target->command)
    {
    case CommandBind:
        self->bind();
        break;
    case CommandUnbind:
        self->unbind();
        break;
    case CommandUninstall:
        self->uninstall();
        break;
    case CommandShowPilot:
    {
        int count = 0;
        SDL_Window** windows = SDL_GetWindows(&count);
        for (int i = 0; i < count; ++i)
        {
            SDL_RestoreWindow(windows[i]);
            SDL_RaiseWindow(windows[i]);
        }
        SDL_free(windows);
        break;
    }
    case CommandLogs:
        reveal(stateFolder());
        break;
    case CommandRuntimeFolder:
        reveal(native(fs::path(installedRuntimeManifest()).parent_path().string()));
        break;
    case CommandDriverFolder:
        reveal(installedDriverFolder());
        break;
    case CommandDeveloperMode:
        setSettingFlag(L"developerMode", SDL_GetTrayEntryChecked(self->developerMode_));
        self->rebuildDeveloperEntries();
        break;
    case CommandAddRuntime:
        self->addRuntimeManifest();
        break;
    case CommandQuit:
    {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
        break;
    }
    }
}
