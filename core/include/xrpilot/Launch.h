// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// What one start of xr-pilot does, from its arguments and whether a copy already runs on the desk.

#pragma once

#include <string>

namespace xrpilot
{

enum class LaunchMode
{
    // Opens the pilot; closing it leaves the tray running. What the Start Menu entry does.
    PilotThenTray,
    // The tray alone, for a start at sign-in.
    TrayOnly,
    // The pilot alone; closing it quits. For scripts and recordings.
    PilotOnly,
    // A pilot an agent drives over stdin.
    Agent,
    // Asks the copy already running to show its pilot, then quits.
    ShowRunning,
    PrintVersion,
    Usage,
};

struct LaunchOptions
{
    LaunchMode mode = LaunchMode::PilotThenTray;
    bool autoConnect = true;
    std::string snapshotPath;
    std::string recordStreamPath;
    std::string tracePath;
};

// alreadyRunning: another desk copy (not an agent's pilot) holds the tray.
LaunchOptions parseLaunch(int argc, const char* const* argv, bool alreadyRunning);

// Whether this start sets up the desk and holds the tray (agents and scripted pilots do not).
bool ownsDesk(LaunchMode mode);

} // namespace xrpilot
