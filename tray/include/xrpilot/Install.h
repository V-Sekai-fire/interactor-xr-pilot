// SPDX-License-Identifier: MPL-2.0
//
// Installing an OXRSys package or build into the per-user folders the runtime and SteamVR load from,
// ported from OXRSys Home's tray and scripts/windows_build.ps1 -Install.

#pragma once

#include <filesystem>
#include <string>

namespace xrpilot
{

// Whether root holds what an install copies: the runtime DLL and the SteamVR driver's manifest and DLL.
// missing names the first file that is not there.
bool isOxrsysSource(const std::filesystem::path& root, std::string* missing = nullptr);

// Whether base holds a whole install: the runtime manifest and DLL, and the driver's manifest and DLL.
bool isOxrsysInstalled(const std::filesystem::path& base);

// Copies the runtime and driver from root into base and writes the runtime manifest, then checks each
// installed file is identical to its source. Nothing is written unless root is complete; false with
// error when a file cannot be replaced, as when SteamVR has the driver loaded.
bool installOxrsys(const std::filesystem::path& root, const std::filesystem::path& base, std::string* error);

} // namespace xrpilot
