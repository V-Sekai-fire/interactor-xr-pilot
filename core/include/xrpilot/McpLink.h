// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The desk copy's MCP server (xr_pilot_mcp --http --parent) and the pilot commands it sends back here.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace xrpilot
{

// Past OXRSys's ports, 9943 to 9948.
constexpr int DefaultMcpPort = 9949;

// Reads the token at path, or writes 32 bytes from the OS's CSPRNG there, as hex, readable by this user alone.
bool loadOrCreateMcpToken(const std::string& path, std::string& token, std::string* error = nullptr);

std::string mcpClientConfig(int port, const std::string& token);

// Hands each command to the open pilot; with none open, route asks for one and waits for it.
class CommandRouter
{
public:
    using Handler = std::function<std::string(const std::string&)>;

    void attach(Handler handler);
    void detach();
    void close();
    std::string route(const std::string& line, const std::function<void()>& openPilot, std::chrono::milliseconds wait);

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    Handler handler_;
    bool busy_ = false;
    bool closed_ = false;
};

} // namespace xrpilot
