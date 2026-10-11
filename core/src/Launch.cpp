// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Launch.h"

#include <cstdlib>
#include <cstring>

namespace xrpilot
{

LaunchOptions parseLaunch(int argc, const char* const* argv, bool alreadyRunning)
{
    LaunchOptions o;
    bool tray = false;
    bool window = false;
    bool agent = false;
    bool mcpConfig = false;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (!std::strcmp(a, "--version"))
        {
            o.mode = LaunchMode::PrintVersion;
            return o;
        }
        if (!std::strcmp(a, "--snapshot") && hasValue)
            o.snapshotPath = argv[++i];
        else if (!std::strcmp(a, "--record-stream") && hasValue)
            o.recordStreamPath = argv[++i];
        else if (!std::strcmp(a, "--traces") && hasValue)
            o.tracePath = argv[++i];
        else if (!std::strcmp(a, "--mcp-port") && hasValue)
        {
            char* end = nullptr;
            const long port = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || port < 0 || port > 65535)
            {
                o.mode = LaunchMode::Usage;
                return o;
            }
            o.mcpPort = int(port);
        }
        else if (!std::strcmp(a, "--mcp-config"))
            mcpConfig = true;
        else if (!std::strcmp(a, "--no-autoconnect"))
            o.autoConnect = false;
        else if (!std::strcmp(a, "--agent"))
            agent = true;
        else if (!std::strcmp(a, "--window"))
            window = true;
        else if (!std::strcmp(a, "--tray"))
            tray = true;
        else
        {
            o.mode = LaunchMode::Usage;
            return o;
        }
    }
    const bool scripted = window || !o.snapshotPath.empty();
    if ((tray && (scripted || agent)) || (mcpConfig && (tray || scripted || agent)))
        o.mode = LaunchMode::Usage;
    else if (mcpConfig)
        o.mode = LaunchMode::PrintMcpConfig;
    else if (agent)
        o.mode = LaunchMode::Agent;
    else if (scripted)
        o.mode = LaunchMode::PilotOnly;
    else if (alreadyRunning)
        o.mode = LaunchMode::ShowRunning;
    else
        o.mode = tray ? LaunchMode::TrayOnly : LaunchMode::PilotThenTray;
    return o;
}

bool ownsDesk(LaunchMode mode)
{
    return mode == LaunchMode::PilotThenTray || mode == LaunchMode::TrayOnly;
}

} // namespace xrpilot
