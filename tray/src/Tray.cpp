// SPDX-License-Identifier: MPL-2.0

#include "xrpilot/Tray.h"

#include "xrpilot/Install.h"
#include "xrpilot/Json.h"
#include "xrpilot/SteamVrSettings.h"
#include "xrpilot/StreamTracks.h"
#include "xrpilot/UiTrace.h"
#include "xrpilot/Version.h"

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

const wchar_t* const TrayMutexName = L"Local\\OXRSysTray";
const wchar_t* const ShowPilotEventName = L"Local\\XRPilotShowPilot";

void* holdTrayMutex() { return CreateMutexW(nullptr, FALSE, TrayMutexName); }
void releaseTrayMutex(void* handle)
{
    if (handle != nullptr)
        CloseHandle(handle);
}
bool trayMutexHeld()
{
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, TrayMutexName);
    if (h == nullptr)
        return false;
    CloseHandle(h);
    return true;
}
void* openShowRequest() { return CreateEventW(nullptr, FALSE, FALSE, ShowPilotEventName); }
bool signalShowRequest()
{
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, ShowPilotEventName);
    if (h == nullptr)
        return false;
    const bool ok = SetEvent(h) != 0;
    CloseHandle(h);
    return ok;
}
bool takeShowRequest(void* handle) { return handle != nullptr && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0; }

std::string pilotExePath()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path, int(length), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, int(length), out.data(), bytes, nullptr, nullptr);
    return out;
}

void recordPilotPath()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        RegSetKeyValueW(HKEY_CURRENT_USER, SettingsKey, L"pilotPath", REG_SZ, path, DWORD((length + 1) * sizeof(wchar_t)));
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
void* holdTrayMutex() { return nullptr; }
void releaseTrayMutex(void*) {}
bool trayMutexHeld() { return false; }
std::string pilotExePath() { return {}; }
void* openShowRequest() { return nullptr; }
bool signalShowRequest() { return false; }
bool takeShowRequest(void*) { return false; }
void recordPilotPath() {}

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

// The runtime's config file, where Config::ResolveConfigFilePath looks first (RuntimePlatform's ConfigRoot).
fs::path runtimeConfigPath()
{
#if defined(_WIN32)
    return fs::path(native(environment("APPDATA") + "/OXRSys/oxrsys-runtime.toml"));
#elif defined(__APPLE__)
    return fs::path(environment("HOME") + "/Library/Application Support/OXRSys/oxrsys-runtime.toml");
#else
    const std::string xdg = environment("XDG_CONFIG_HOME");
    return fs::path((xdg.empty() ? environment("HOME") + "/.config" : xdg) + "/oxrsys/oxrsys-runtime.toml");
#endif
}

// The config text the runtime reads now: the preferred file, else the one beside the installed runtime,
// which the runtime falls back to; writing the preferred file then keeps that one's other settings.
std::string runtimeConfigText()
{
    for (const fs::path& path : {runtimeConfigPath(), fs::path(native(installBase() + "/runtime/oxrsys-runtime.toml"))})
    {
        std::ifstream in(path, std::ios::binary);
        if (in)
        {
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }
    }
    return {};
}

// A package ships the runtime and driver beside the app, which sits in the package's bin folder.
fs::path packageRoot()
{
    const char* base = SDL_GetBasePath();
    return base != nullptr ? fs::path(base).parent_path().parent_path() : fs::path();
}

// An unpackaged pilot installs the runtime and driver built beside it, in its build's oxrsys folder.
fs::path oxrsysBuild()
{
    const char* base = SDL_GetBasePath();
    return base != nullptr ? fs::path(base) / "oxrsys" : fs::path();
}

// Windows will not load a packaged DLL into another app's process, so a package's runtime and driver
// are copied out to installBase(); unpackaged, they come from the oxrsys build.
bool installRuntime(std::string* error)
{
    const fs::path source = xrpilot::isOxrsysSource(packageRoot()) ? packageRoot() : oxrsysBuild();
    return xrpilot::installOxrsys(source, fs::path(installBase()), error);
}

// Returns why the packaged runtime or driver could not be copied, or nothing. SteamVR holds the driver
// open while it runs, so an update made then would leave the old driver in place, unseen.
std::string installPackagedFiles()
{
    std::string error;
    if (xrpilot::isOxrsysSource(packageRoot()) && !xrpilot::installOxrsys(packageRoot(), fs::path(installBase()), &error))
        return error.empty() ? std::string("the OXRSys files could not be copied") : error;
    return {};
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

// SteamVR picks one headset driver among those that load. Swapping to OXRSys forces ours and turns on
// multiple drivers so the others (a Quest link, trackers) still load; handing back undoes both, multiple
// drivers to what the desk had (remembered across starts). Returns whether the file changed.
bool useSteamVrHeadset(bool ours)
{
    const std::string config = firstString(openVrPaths().find("config"));
    if (config.empty())
        return false;
    const fs::path path = fs::path(config) / "steamvr.vrsettings";
    Json settings = readJson(path);
    xrpilot::MultipleDriversBefore before;
    if (settingFlag(L"multipleDriversSaved"))
        before = settingFlag(L"multipleDriversWas");
    if (!xrpilot::applySteamVrHeadset(settings, ours, before))
        return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << xrpilot::writeJson(settings);
    setSettingFlag(L"multipleDriversSaved", before.has_value());
    setSettingFlag(L"multipleDriversWas", before.value_or(false));
    return bool(file);
}

// Whether steamvr.vrsettings makes OXRSys SteamVR's headset.
bool steamVrForcesOxrsys()
{
    const std::string config = firstString(openVrPaths().find("config"));
    if (config.empty())
        return false;
    const Json settings = readJson(fs::path(config) / "steamvr.vrsettings");
    const Json* steamvr = settings.find("steamvr");
    const Json* forced = steamvr != nullptr ? steamvr->find("forcedDriver") : nullptr;
    return forced != nullptr && forced->str() == "oxrsys";
}

// SteamVR's own OpenXR runtime manifest, the way back when the desk had no runtime before OXRSys.
std::string steamVrRuntimeManifest()
{
    const std::string steamVr = firstString(openVrPaths().find("runtime"));
    if (steamVr.empty())
        return {};
    const std::string manifest = native(steamVr + "/steamxr_win64.json");
    std::error_code error;
    return fs::exists(fs::path(manifest), error) ? manifest : std::string();
}

std::string runtimeName(const std::string& manifest)
{
    const Json j = readJson(manifest);
    const Json* runtime = j.find("runtime");
    const std::string name = runtime != nullptr && runtime->find("name") != nullptr ? runtime->find("name")->str() : "";
    return name.empty() ? fs::path(manifest).stem().string() : name;
}

// Quotes for a single-quoted PowerShell string. Not named quoted: std::quoted wins for a non-const string.
std::string psQuoted(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += c == '\'' ? std::string("''") : std::string(1, c);
    return out;
}

// A runtime on another machine reaches the pilot on the discovery and video ports. Windows asks once
// per program path, and a dismissed prompt blocks it unseen, so binding allows them for the subnet.
const char* const FirewallRule = "XR Pilot stream";

std::string firewallScript(bool allow)
{
    std::string script = std::string("Remove-NetFirewallRule -DisplayName '") + FirewallRule + "' -ErrorAction SilentlyContinue; ";
    const std::string exe = pilotExePath();
    if (allow && !exe.empty())
        script += std::string("New-NetFirewallRule -DisplayName '") + FirewallRule + "' -Direction Inbound -Action Allow -Program '" +
                  psQuoted(exe) + "' -Protocol UDP -LocalPort 9943,9944 -RemoteAddress LocalSubnet -Profile Any | Out-Null; ";
    return script;
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
    CommandModePcvr,
    CommandModeSteamVr,
    CommandModeStream,
    CommandQuit,
};

struct CommandTarget
{
    Tray* tray;
    Command command;
};

struct TrackTarget
{
    Tray* tray;
    bool abr;
    size_t index;
};

std::vector<std::unique_ptr<TrackTarget>>& trackTargets()
{
    static std::vector<std::unique_ptr<TrackTarget>> targets;
    return targets;
}

std::vector<std::unique_ptr<CommandTarget>>& commandTargets()
{
    static std::vector<std::unique_ptr<CommandTarget>> targets;
    return targets;
}

} // namespace

Tray::Tray(bool setUpDesk, xrpilot::UiTrace* ui)
    : ui_(ui)
{
    // Only the desk copy holds the tray: an agent's or a script's pilot never shows a second icon whose
    // Bind, Unbind and Quit would act from the wrong process.
    if (!setUpDesk)
        return;
    {
        instance_ = holdTrayMutex();
        showRequest_ = openShowRequest();
        recordPilotPath();
        const std::string installError = installPackagedFiles();
        registerSteamVrDriver();
        record(xrpilot::UiAction::DeskSetup);
        if (!installError.empty())
        {
            record(xrpilot::UiAction::UpdateWarning, false, installError);
            const std::string text = "OXRSys was not updated: " + installError +
                                     "\n\nQuit SteamVR, then start XR Pilot again.";
            setMessage("OXRSys not updated: quit SteamVR and start XR Pilot again");
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "XR Pilot", text.c_str(), nullptr);
        }
        // A first start binds OXRSys once (one administrator prompt), so SteamVR streams its headset
        // to the pilot rather than to another driver. An unbind or a later runtime pick is kept.
        if (!settingFlag(L"firstRunDone"))
        {
            setSettingFlag(L"firstRunDone", true);
            if (bindingWanted() && !sameText(native(openXrValue(L"ActiveRuntime")), installedRuntimeManifest()))
            {
                record(xrpilot::UiAction::FirstRunBind);
                bind();
            }
        }
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
    SDL_TrayMenu* modeMenu = SDL_CreateTraySubmenu(SDL_InsertTrayEntryAt(menu_, -1, "Mode", SDL_TRAYENTRY_SUBMENU));
    const std::pair<const char*, Command> modes[] = {
        {"PCVR on OXRSys", CommandModePcvr},
        {"SteamVR on OXRSys", CommandModeSteamVr},
        {"Stream to a headset (Steam Frame, Quest 3)", CommandModeStream},
    };
    for (size_t i = 0; i < 3; ++i)
    {
        modeEntries_[i] = SDL_InsertTrayEntryAt(modeMenu, -1, modes[i].first, SDL_TRAYENTRY_CHECKBOX);
        commandTargets().push_back(std::make_unique<CommandTarget>(CommandTarget{this, modes[i].second}));
        SDL_SetTrayEntryCallback(modeEntries_[i], &Tray::onCommand, commandTargets().back().get());
    }

    // The stream's bitrate and adaptive bitrate, as named tracks.
    SDL_TrayMenu* bitrateMenu = SDL_CreateTraySubmenu(SDL_InsertTrayEntryAt(menu_, -1, "Bitrate", SDL_TRAYENTRY_SUBMENU));
    for (size_t i = 0; i < xrpilot::bitrateTracks().size(); ++i)
    {
        bitrateEntries_.push_back(
            SDL_InsertTrayEntryAt(bitrateMenu, -1, xrpilot::bitrateTracks()[i].name, SDL_TRAYENTRY_CHECKBOX));
        trackTargets().push_back(std::make_unique<TrackTarget>(TrackTarget{this, false, i}));
        SDL_SetTrayEntryCallback(bitrateEntries_.back(), &Tray::onTrack, trackTargets().back().get());
    }
    SDL_TrayMenu* abrMenu =
        SDL_CreateTraySubmenu(SDL_InsertTrayEntryAt(menu_, -1, "Adaptive bitrate", SDL_TRAYENTRY_SUBMENU));
    for (size_t i = 0; i < xrpilot::abrTracks().size(); ++i)
    {
        abrEntries_.push_back(SDL_InsertTrayEntryAt(abrMenu, -1, xrpilot::abrTracks()[i].name, SDL_TRAYENTRY_CHECKBOX));
        trackTargets().push_back(std::make_unique<TrackTarget>(TrackTarget{this, true, i}));
        SDL_SetTrayEntryCallback(abrEntries_.back(), &Tray::onTrack, trackTargets().back().get());
    }

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
    releaseTrayMutex(instance_);
    releaseTrayMutex(showRequest_);
    if (tray_ != nullptr)
        SDL_DestroyTray(tray_);
    SDL_DestroySurface(idleIcon_);
    SDL_DestroySurface(streamingIcon_);
}

void Tray::record(xrpilot::UiAction action, bool ok, const std::string& detail)
{
    if (ui_ != nullptr)
        ui_->record(action, ok, detail);
}

void Tray::poll()
{
    if (takeShowRequest(showRequest_))
    {
        record(xrpilot::UiAction::ShowRequest);
        showPilot();
    }
    if (tray_ != nullptr)
        refresh();
}

bool Tray::deskCopyRunning()
{
    return trayMutexHeld();
}

bool Tray::askRunningToShowPilot()
{
    return signalShowRequest();
}

void Tray::showPilot()
{
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (count == 0)
    {
        SDL_Event show{};
        show.type = showPilotEvent();
        SDL_PushEvent(&show);
    }
    for (int i = 0; i < count; ++i)
    {
        SDL_RestoreWindow(windows[i]);
        SDL_RaiseWindow(windows[i]);
    }
    SDL_free(windows);
}

void Tray::refresh()
{
    bool streaming = false;
    const std::string status = activityText(streaming);
    SDL_SetTrayEntryLabel(status_, status.c_str());
    SDL_SetTrayTooltip(tray_, ("XR Pilot " + std::string(xrpilot::Version) + "\nOXRSys: " + status).c_str());
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
    refreshModeEntries();
    refreshTrackEntries();
}

// Checks the tracks the runtime's config is on; an unset value shows the runtime's default (50 Mbps, bitrate).
void Tray::refreshTrackEntries()
{
    const std::string text = runtimeConfigText();
    const std::string mbps = xrpilot::tomlValue(text, "streaming", "bitrate_mbps");
    const std::string mode = xrpilot::tomlValue(text, "streaming", "abr_mode");
    const size_t bitrate = xrpilot::bitrateTrackFor(mbps.empty() ? 50u : uint32_t(std::strtoul(mbps.c_str(), nullptr, 10)));
    const size_t abr = xrpilot::abrTrackFor(mode.empty() ? "bitrate" : mode);
    for (size_t i = 0; i < bitrateEntries_.size(); ++i)
        SDL_SetTrayEntryChecked(bitrateEntries_[i], i == bitrate);
    for (size_t i = 0; i < abrEntries_.size(); ++i)
        SDL_SetTrayEntryChecked(abrEntries_[i], i == abr);
}

// Writes the picked track into the runtime's config; the runtime reads it when a headset next connects.
void Tray::pickTrack(bool abr, size_t index)
{
    const std::string key = abr ? "abr_mode" : "bitrate_mbps";
    const std::string value = abr ? "\"" + std::string(xrpilot::abrTracks()[index].mode) + "\""
                                  : std::to_string(xrpilot::bitrateTracks()[index].mbps);
    const std::string name = abr ? xrpilot::abrTracks()[index].name : xrpilot::bitrateTracks()[index].name;
    const fs::path path = runtimeConfigPath();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const std::string text = xrpilot::setTomlValue(runtimeConfigText(), "streaming", key, value);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    const bool ok = bool(out);
    record(abr ? xrpilot::UiAction::StreamAbr : xrpilot::UiAction::StreamBitrate, ok, name);
    setMessage(ok ? std::string(abr ? "Adaptive bitrate" : "Bitrate") + ": " + name +
                        ". The headset gets it when it next connects."
                  : "Could not write " + path.string());
    refreshTrackEntries();
}

void Tray::onTrack(void* userdata, SDL_TrayEntry*)
{
    const TrackTarget* target = static_cast<const TrackTarget*>(userdata);
    target->tray->pickTrack(target->abr, target->index);
}

// The mode the desk is in, as lean/XrPilot/Ui.lean's Desk.inMode reads it: OXRSys the runtime and
// SteamVR's headset; SteamVR the runtime with OXRSys its headset, the pilot owning or watching.
void Tray::refreshModeEntries()
{
    const std::string active = native(openXrValue(L"ActiveRuntime"));
    const bool forced = steamVrForcesOxrsys();
    const bool onSteamVr = !active.empty() && sameText(active, steamVrRuntimeManifest());
    const bool in[3] = {
        forced && sameText(active, installedRuntimeManifest()) && !streamsToHeadset(),
        forced && onSteamVr && !streamsToHeadset(),
        forced && onSteamVr && streamsToHeadset(),
    };
    for (size_t i = 0; i < 3; ++i)
    {
        if (modeEntries_[i] != nullptr)
            SDL_SetTrayEntryChecked(modeEntries_[i], in[i]);
    }
}

bool Tray::streamsToHeadset()
{
    return settingFlag(L"streamToHeadset");
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
    std::string script = (ours ? firewallScript(true) : std::string()) + "New-Item -Force -Path '" + key + "\\AvailableRuntimes' | Out-Null; Set-ItemProperty -Path '" + key +
                         "\\AvailableRuntimes' -Name '" + psQuoted(manifest) + "' -Value 0 -Type DWord; ";
    if (!active.empty() && !sameText(active, manifest))
        script += "Set-ItemProperty -Path '" + key + "' -Name PreviousActiveRuntime -Value '" + psQuoted(active) + "'; ";
    script += "Set-ItemProperty -Path '" + key + "' -Name ActiveRuntime -Value '" + psQuoted(manifest) + "'";
    if (!runElevatedPowerShell(script))
    {
        setMessage("Default runtime unchanged: the administrator prompt was declined");
        record(xrpilot::UiAction::ElevatedDeclined, false, runtimeName(manifest));
        return;
    }
    const bool headsetChanged = useSteamVrHeadset(ours);
    setSettingFlag(L"streamToHeadset", false);
    record(xrpilot::UiAction::ElevatedDone, true, runtimeName(manifest));
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
        record(xrpilot::UiAction::InstallFailed, false, error);
        return;
    }
    shownRuntimes_.clear();
    makeDefaultRuntimeNow(installedRuntimeManifest());
}

// SteamVR stays (or becomes) the OpenXR runtime, and OXRSys is its headset with multiple drivers on.
// Installs OXRSys first, as Bind does; the prompt comes only when SteamVR is not already the runtime.
void Tray::useSteamVrModeNow(const std::string& mode)
{
    std::string error;
    const std::string manifest = steamVrRuntimeManifest();
    if (manifest.empty())
        error = "SteamVR's OpenXR runtime was not found";
    if (error.empty() && !installRuntime(&error))
        error = error.empty() ? "the runtime did not install" : error;
    if (!error.empty())
    {
        setMessage("Not installed: " + error);
        record(xrpilot::UiAction::InstallFailed, false, error);
        return;
    }
    shownRuntimes_.clear();
    setSettingFlag(L"unbound", false);
    registerSteamVrDriver();
    const std::string key = "HKLM:\\SOFTWARE\\Khronos\\OpenXR\\1";
    const std::string active = openXrValue(L"ActiveRuntime");
    if (!sameText(native(active), manifest))
    {
        std::string script;
        if (!active.empty())
            script += "Set-ItemProperty -Path '" + key + "' -Name PreviousActiveRuntime -Value '" + psQuoted(active) + "'; ";
        script += "Set-ItemProperty -Path '" + key + "' -Name ActiveRuntime -Value '" + psQuoted(manifest) + "'";
        if (!runElevatedPowerShell(script))
        {
            setMessage("Mode unchanged: the administrator prompt was declined");
            record(xrpilot::UiAction::ElevatedDeclined, false, mode);
            return;
        }
    }
    const bool headsetChanged = useSteamVrHeadset(true);
    const bool stream = mode == "stream";
    setSettingFlag(L"streamToHeadset", stream);
    record(xrpilot::UiAction::ElevatedDone, true, mode);
    setMessage(std::string(stream ? "Streaming to a headset: start OXRSys on it" : "SteamVR on OXRSys") +
               (headsetChanged ? "; restart SteamVR for its headset to follow" : ""));
}

void Tray::unbind()
{
    setMessage("Unbinding...");
    runInBackground(&Tray::unbindNow, std::string());
}

void Tray::unbindNow(const std::string&)
{
    unbindQuietly(true);
}

// Undoes everything binding did: the OpenXR default goes back to the runtime before OXRSys (SteamVR's
// when there was none), ours leaves the loader's list, the driver leaves SteamVR's list, and SteamVR's
// headset and multiple drivers are handed back. Picking OXRSys in the runtime menu binds it again.
void Tray::unbindQuietly(bool recordIt)
{
    setSettingFlag(L"unbound", true);
    const int drivers = unregisterSteamVrDriver();
    useSteamVrHeadset(false);
    std::string runtime = "left as it was";
    bool declined = false;
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
        std::string script = firewallScript(false) + "Remove-ItemProperty -Path '" + key + "\\AvailableRuntimes' -Name '" + psQuoted(ours) +
                             "' -ErrorAction SilentlyContinue; ";
        if (previousIsOurs)
            script += "Remove-ItemProperty -Path '" + key + "' -Name PreviousActiveRuntime -ErrorAction SilentlyContinue; ";
        std::error_code error;
        const bool previousUsable = !previous.empty() && !previousIsOurs && fs::exists(fs::path(previous), error);
        const std::string back = previousUsable ? previous : steamVrRuntimeManifest();
        const bool restore = activeIsOurs && !back.empty();
        if (restore)
            script += "Set-ItemProperty -Path '" + key + "' -Name ActiveRuntime -Value '" + psQuoted(back) + "'";
        else if (activeIsOurs)
            script += "Remove-ItemProperty -Path '" + key + "' -Name ActiveRuntime -ErrorAction SilentlyContinue";
        declined = !runElevatedPowerShell(script);
        if (!declined)
            runtime = restore ? "back to " + runtimeName(back) : activeIsOurs ? "cleared" : "left as it was";
        else
            runtime = "unchanged: the administrator prompt was declined";
    }
    if (!declined)
        setSettingFlag(L"streamToHeadset", false);
    if (recordIt)
        record(declined ? xrpilot::UiAction::ElevatedDeclined : xrpilot::UiAction::ElevatedDone, !declined, runtime);
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
    {
        record(xrpilot::UiAction::UninstallCancel);
        return;
    }
    record(xrpilot::UiAction::UninstallConfirm);
    unbindQuietly(false);
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
    self->record(xrpilot::UiAction::AddRuntimeChosen, true, native(files[0]));
    std::vector<std::string> known = settingList(L"knownRuntimes");
    known.push_back(native(files[0]));
    setSettingList(L"knownRuntimes", known);
    self->shownRuntimes_.clear();
}

Uint32 Tray::showPilotEvent()
{
    static const Uint32 type = SDL_RegisterEvents(1);
    return type;
}

void Tray::onRuntime(void* userdata, SDL_TrayEntry*)
{
    const Choice* choice = static_cast<const Choice*>(userdata);
    // Picking OXRSys is the swap to it; picking SteamVR (or another runtime) is the way back.
    const bool ours = sameText(choice->manifest, installedRuntimeManifest());
    choice->tray->record(ours ? xrpilot::UiAction::RuntimeOurs : xrpilot::UiAction::RuntimeOther, true,
                         runtimeName(choice->manifest));
    choice->tray->makeDefaultRuntime(choice->manifest);
}

void Tray::onCommand(void* userdata, SDL_TrayEntry*)
{
    const CommandTarget* target = static_cast<const CommandTarget*>(userdata);
    Tray* self = target->tray;
    switch (target->command)
    {
    case CommandBind:
        self->record(xrpilot::UiAction::TrayBind);
        self->bind();
        break;
    case CommandUnbind:
        self->record(xrpilot::UiAction::TrayUnbind);
        self->unbind();
        break;
    case CommandUninstall:
        self->record(xrpilot::UiAction::TrayUninstall);
        self->uninstall();
        break;
    case CommandShowPilot:
        self->record(xrpilot::UiAction::TrayShowPilot);
        showPilot();
        break;
    case CommandLogs:
        self->record(xrpilot::UiAction::OpenLogs);
        reveal(stateFolder());
        break;
    case CommandRuntimeFolder:
        self->record(xrpilot::UiAction::OpenRuntimeFolder);
        reveal(native(fs::path(installedRuntimeManifest()).parent_path().string()));
        break;
    case CommandDriverFolder:
        self->record(xrpilot::UiAction::OpenDriverFolder);
        reveal(installedDriverFolder());
        break;
    case CommandDeveloperMode:
    {
        const bool on = SDL_GetTrayEntryChecked(self->developerMode_);
        self->record(xrpilot::UiAction::DeveloperMode, true, on ? "on" : "off");
        setSettingFlag(L"developerMode", on);
        self->rebuildDeveloperEntries();
        break;
    }
    case CommandModePcvr:
        self->record(xrpilot::UiAction::ModePcvr);
        self->bind();
        break;
    case CommandModeSteamVr:
        self->record(xrpilot::UiAction::ModeSteamVr);
        self->setMessage("Installing OXRSys...");
        self->runInBackground(&Tray::useSteamVrModeNow, "steamvr");
        break;
    case CommandModeStream:
        self->record(xrpilot::UiAction::ModeStream);
        self->setMessage("Installing OXRSys...");
        self->runInBackground(&Tray::useSteamVrModeNow, "stream");
        break;
    case CommandAddRuntime:
        self->record(xrpilot::UiAction::AddRuntime);
        self->addRuntimeManifest();
        break;
    case CommandQuit:
    {
        self->record(xrpilot::UiAction::TrayQuit);
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
        break;
    }
    }
}
