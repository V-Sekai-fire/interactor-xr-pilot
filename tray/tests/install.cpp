// SPDX-License-Identifier: MPL-2.0
//
// Install tests, one case per ctest entry: a complete source installs identical files, and each broken
// source or partial install is refused.

#include "xrpilot/Install.h"
#include "xrpilot/Json.h"

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
