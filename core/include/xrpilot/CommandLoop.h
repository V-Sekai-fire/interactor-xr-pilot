// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The pilot's commands from its stdin under --agent, or from the tray's MCP server through the bridge.

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <string>

namespace xrpilot
{

using Answer = std::function<std::string(const std::string&)>;

// Strips a trailing CR and a leading UTF-8 byte order mark; false for a blank line.
bool normalizeCommandLine(std::string& line);

void runCommandLoop(std::istream& in, std::ostream& out, const Answer& answer);

class CommandBridge final
{
public:
    explicit CommandBridge(std::function<void()> wake);

    CommandBridge(const CommandBridge&) = delete;
    CommandBridge& operator=(const CommandBridge&) = delete;

    void attach(Answer answer);
    // Returns once the command in flight, if any, has its reply.
    void detach();
    bool attached();

    // With no pilot attached, wakes the tray and waits up to timeout for one.
    std::string ask(const std::string& line, std::chrono::milliseconds timeout);

private:
    std::function<void()> wake_;
    std::mutex mutex_;
    std::condition_variable changed_;
    Answer answer_;
    int inFlight_ = 0;
};

} // namespace xrpilot
