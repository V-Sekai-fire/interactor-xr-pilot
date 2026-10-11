// SPDX-License-Identifier: MPL-2.0
//
// Install tests, one case per ctest entry: a complete source installs identical files, and each broken
// source or partial install is refused.

#include "xrpilot/Install.h"
#include "xrpilot/Json.h"
#include "xrpilot/SteamVrSettings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace xrpilot;
namespace fs = std::filesystem;

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void write(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
}

std::string read(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// A fresh scratch folder with a complete source under src/ and an empty install target dst/.
struct Scratch
{
    fs::path root;
    fs::path src;
    fs::path dst;

    explicit Scratch(const char* name)
    {
        root = fs::temp_directory_path() / (std::string("xrpilot-install-") + name);
        fs::remove_all(root);
        src = root / "src";
        dst = root / "dst";
        write(src / "runtime/liboxrsys-runtime.dll", "runtime-dll-v1");
        write(src / "driver/oxrsys/driver.vrdrivermanifest", "{\"name\":\"oxrsys\"}");
        write(src / "driver/oxrsys/bin/win64/driver_oxrsys.dll", "driver-dll-v1");
        write(src / "driver/oxrsys/resources/settings/default.vrsettings", "{}");
    }
    ~Scratch() { fs::remove_all(root); }
};

const std::map<std::string, std::function<void()>> cases = {
    {"install.complete-source-installs",
     [] {
         Scratch s("complete");
         std::string error;
         check(installOxrsys(s.src, s.dst, &error), "a complete source installs");
         check(isOxrsysInstalled(s.dst), "the install is whole");
         check(fs::exists(s.dst / "driver/oxrsys/resources/settings/default.vrsettings"), "driver resources come too");
         Json manifest;
         check(parseJson(read(s.dst / "runtime/oxrsys-runtime.json"), manifest), "the runtime manifest is JSON");
         const Json* runtime = manifest.find("runtime");
         check(runtime && runtime->find("name") && runtime->find("name")->str() == "OXRSys Runtime",
               "the manifest names the runtime");
     }},
    {"install.files-identical-to-source",
     [] {
         Scratch s("identity");
         installOxrsys(s.src, s.dst, nullptr);
         for (const char* f : {"runtime/liboxrsys-runtime.dll", "driver/oxrsys/driver.vrdrivermanifest",
                               "driver/oxrsys/bin/win64/driver_oxrsys.dll"})
             check(read(s.src / f) == read(s.dst / f), f);
         Json manifest;
         parseJson(read(s.dst / "runtime/oxrsys-runtime.json"), manifest);
         const std::string library = manifest.find("runtime")->find("library_path")->str();
         check(library == ".\\liboxrsys-runtime.dll" && fs::exists(s.dst / "runtime" / "liboxrsys-runtime.dll"),
               "the manifest's library_path is the installed DLL beside it");
     }},
    {"install.changed-source-is-recopied",
     [] {
         Scratch s("changed");
         installOxrsys(s.src, s.dst, nullptr);
         const fs::path dll = s.src / "driver/oxrsys/bin/win64/driver_oxrsys.dll";
         const fs::file_time_type stamp = fs::last_write_time(s.dst / "driver/oxrsys/bin/win64/driver_oxrsys.dll");
         write(dll, "driver-dll-v2"); // same size as v1
         fs::last_write_time(dll, stamp);
         check(installOxrsys(s.src, s.dst, nullptr), "reinstalling succeeds");
         check(read(s.dst / "driver/oxrsys/bin/win64/driver_oxrsys.dll") == "driver-dll-v2",
               "a source change of the same size and time still reaches the install");
     }},
    {"install.missing-driver-writes-nothing",
     [] {
         Scratch s("no-driver");
         fs::remove(s.src / "driver/oxrsys/bin/win64/driver_oxrsys.dll");
         std::string error;
         check(!installOxrsys(s.src, s.dst, &error), "a source without the driver DLL is refused");
         check(error.find("driver_oxrsys.dll") != std::string::npos, "the error names the missing file");
         check(!fs::exists(s.dst), "nothing is written");
     }},
    {"install.missing-runtime-refused",
     [] {
         Scratch s("no-runtime");
         fs::remove(s.src / "runtime/liboxrsys-runtime.dll");
         check(!installOxrsys(s.src, s.dst, nullptr), "a source without the runtime DLL is refused");
         check(!isOxrsysSource(s.root / "nowhere"), "a folder that does not exist is no source");
     }},
    {"steamvr.swap-forces-oxrsys-with-multiple-drivers",
     [] {
         // Desk.swapToOxrsys: OXRSys is SteamVR's headset, and SteamVR's other drivers (a Quest link,
         // trackers) still load.
         Json settings;
         check(parseJson("{\"steamvr\":{\"enableHomeApp\":false},\"driver_null\":{\"enable\":false}}", settings), "parses");
         MultipleDriversBefore before;
         check(applySteamVrHeadset(settings, true, before), "the swap changes the settings");
         const Json* sv = settings.find("steamvr");
         check(sv && sv->find("forcedDriver") && sv->find("forcedDriver")->str() == "oxrsys", "forcedDriver is oxrsys");
         check(sv && sv->find("activateMultipleDrivers") && sv->find("activateMultipleDrivers")->boolean, "multiple drivers is on");
         check(before.has_value() && !*before, "multiple drivers was off before the swap");
         check(sv && sv->find("enableHomeApp") && settings.find("driver_null"), "everything else is kept");
         const Json* drv = settings.find("driver_oxrsys");
         check(drv && drv->find("enable") && drv->find("enable")->boolean, "OXRSys's driver is on");
         check(!applySteamVrHeadset(settings, true, before), "a second swap changes nothing");
         check(before.has_value() && !*before, "and still remembers the desk's own value");
     }},
    {"steamvr.hand-back-restores-the-desk",
     [] {
         // Desk.handBackHeadset: SteamVR picks its own headset, and multiple drivers is as the desk had it.
         for (const bool had : {false, true})
         {
             Json settings;
             check(parseJson(had ? "{\"steamvr\":{\"activateMultipleDrivers\":true}}" : "{\"steamvr\":{}}", settings), "parses");
             const std::string original = writeJson(settings);
             MultipleDriversBefore before;
             applySteamVrHeadset(settings, true, before);
             check(applySteamVrHeadset(settings, false, before), "the hand-back changes the settings");
             const Json* sv = settings.find("steamvr");
             check(sv && !sv->find("forcedDriver"), "no forced headset");
             const Json* multi = sv ? sv->find("activateMultipleDrivers") : nullptr;
             check(had ? (multi && multi->boolean) : multi == nullptr, "multiple drivers is as the desk had it");
             check(!before.has_value(), "nothing is remembered after");
             const Json* drv = settings.find("driver_oxrsys");
             check(drv && drv->find("enable") && !drv->find("enable")->boolean, "OXRSys's driver is off");
             settings.erase("driver_oxrsys");
             check(writeJson(settings) == original, "apart from that driver, the round trip leaves the file as it was");
         }
         // A desk that set multiple drivers itself, with no swap in effect: the hand-back leaves it on.
         Json own;
         check(parseJson("{\"steamvr\":{\"activateMultipleDrivers\":true}}", own), "parses");
         MultipleDriversBefore none;
         applySteamVrHeadset(own, false, none);
         const Json* sv = own.find("steamvr");
         check(sv && sv->find("activateMultipleDrivers") && sv->find("activateMultipleDrivers")->boolean,
               "control: the desk's own multiple drivers is not turned off");
     }},
    {"steamvr.oxrsys-driver-follows-the-headset",
     [] {
         // Desk.oxrsysDriver: loaded, OXRSys's driver offers a headset and holds the stream's control port, so it
         // is on only while it is SteamVR's headset. A desk on SteamVR keeps it off for the Steam Frame's Steam
         // Link and a standalone OXRSys runtime; the swap turns it back on, whatever the desk set.
         Json settings;
         check(parseJson("{\"steamvr\":{},\"driver_oxrsys\":{\"enable\":false,\"frameDumpFrame\":3}}", settings), "parses");
         MultipleDriversBefore before;
         check(applySteamVrHeadset(settings, true, before), "the swap changes the settings");
         const Json* drv = settings.find("driver_oxrsys");
         check(drv && drv->find("enable") && drv->find("enable")->boolean, "the swap turns a disabled driver on");
         check(drv && drv->find("frameDumpFrame"), "and keeps the driver's other settings");
         check(applySteamVrHeadset(settings, false, before), "the hand-back changes the settings");
         drv = settings.find("driver_oxrsys");
         check(drv && drv->find("enable") && !drv->find("enable")->boolean, "the hand-back turns it off");
         check(!applySteamVrHeadset(settings, false, before), "a second hand-back changes nothing");
         // A desk already on SteamVR, never swapped, with the driver left on (tonight's desk): the way back
         // still turns it off.
         Json desk;
         check(parseJson("{\"steamvr\":{\"activateMultipleDrivers\":true}}", desk), "parses");
         MultipleDriversBefore none;
         check(applySteamVrHeadset(desk, false, none), "control: a desk with the driver on is changed");
         drv = desk.find("driver_oxrsys");
         check(drv && drv->find("enable") && !drv->find("enable")->boolean, "and its OXRSys driver is off");
     }},
    {"install.stale-manifest-is-not-an-install",
     [] {
         Scratch s("stale");
         write(s.dst / "runtime/oxrsys-runtime.json", "{}");
         check(!isOxrsysInstalled(s.dst), "a manifest left without its DLL and driver is not an install");
         installOxrsys(s.src, s.dst, nullptr);
         fs::remove(s.dst / "driver/oxrsys/driver.vrdrivermanifest");
         check(!isOxrsysInstalled(s.dst), "an install missing the driver manifest is not whole");
     }},
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || cases.count(argv[1]) == 0)
    {
        std::fprintf(stderr, "usage: xrpilot-install-tests <case>\n");
        return 2;
    }
    cases.at(argv[1])();
    return failures == 0 ? 0 : 1;
}
