// SPDX-License-Identifier: MPL-2.0

#include "xrpilot/Install.h"

#include "xrpilot/Json.h"

#include <fstream>
#include <iterator>
#include <vector>

namespace xrpilot
{

namespace
{

namespace fs = std::filesystem;

const char* const RuntimeDll = "runtime/liboxrsys-runtime.dll";
const char* const RuntimeManifest = "runtime/oxrsys-runtime.json";
const char* const DriverManifest = "driver/oxrsys/driver.vrdrivermanifest";
const char* const DriverDll = "driver/oxrsys/bin/win64/driver_oxrsys.dll";

std::vector<char> bytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool identical(const fs::path& a, const fs::path& b)
{
    std::error_code error;
    return fs::is_regular_file(a, error) && fs::is_regular_file(b, error) &&
           fs::file_size(a, error) == fs::file_size(b, error) && bytes(a) == bytes(b);
}

// Copies every file under from that differs from its counterpart under to.
bool copyTree(const fs::path& from, const fs::path& to, std::string* error)
{
    std::error_code code;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(from, code))
    {
        if (!entry.is_regular_file())
            continue;
        const fs::path target = to / fs::relative(entry.path(), from);
        if (identical(entry.path(), target))
            continue;
        fs::create_directories(target.parent_path(), code);
        if (!fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, code))
        {
            if (error)
                *error = "cannot replace " + target.string() + ": " + code.message();
            return false;
        }
    }
    return true;
}

} // namespace

bool isOxrsysSource(const fs::path& root, std::string* missing)
{
    for (const char* file : {RuntimeDll, DriverManifest, DriverDll})
    {
        std::error_code error;
        if (!fs::is_regular_file(root / file, error))
        {
            if (missing)
                *missing = file;
            return false;
        }
    }
    return true;
}

bool isOxrsysInstalled(const fs::path& base)
{
    for (const char* file : {RuntimeManifest, RuntimeDll, DriverManifest, DriverDll})
    {
        std::error_code error;
        if (!fs::is_regular_file(base / file, error))
            return false;
    }
    return true;
}

bool installOxrsys(const fs::path& root, const fs::path& base, std::string* error)
{
    std::string missing;
    if (!isOxrsysSource(root, &missing))
    {
        if (error)
            *error = "no " + missing + " in " + root.string();
        return false;
    }
    if (!copyTree(root / "runtime", base / "runtime", error) ||
        !copyTree(root / "driver" / "oxrsys", base / "driver" / "oxrsys", error))
        return false;

    Json entry;
    entry.set("name", Json::string("OXRSys Runtime"));
    entry.set("library_path", Json::string(".\\liboxrsys-runtime.dll"));
    Json manifest;
    manifest.set("file_format_version", Json::string("1.0.0"));
    manifest.set("runtime", entry);
    std::ofstream(base / RuntimeManifest, std::ios::binary | std::ios::trunc) << writeJson(manifest);

    for (const char* file : {RuntimeDll, DriverManifest, DriverDll})
    {
        if (!identical(root / file, base / file))
        {
            if (error)
                *error = std::string(file) + " differs from its source after install";
            return false;
        }
    }
    return isOxrsysInstalled(base);
}

} // namespace xrpilot
