// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// What an MCP client needs to reach the desk copy's server (xr_pilot_mcp --http --parent).

#pragma once

#include <string>

namespace xrpilot
{

// Past OXRSys's ports, 9943 to 9948.
constexpr int DefaultMcpPort = 9950;

// Reads the token at path, or writes 32 bytes from the OS's CSPRNG there, as hex, readable by this user alone.
bool loadOrCreateMcpToken(const std::string& path, std::string& token, std::string* error = nullptr);

// Claude Code's server entry: the loopback URL and the bearer token.
std::string mcpClientConfig(int port, const std::string& token);

} // namespace xrpilot
