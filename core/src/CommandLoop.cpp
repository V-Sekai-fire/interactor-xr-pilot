// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/CommandLoop.h"

#include "xrpilot/Commands.h"

#include <istream>
#include <ostream>
#include <utility>

namespace xrpilot
{

bool normalizeCommandLine(std::string& line)
{
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.rfind("\xEF\xBB\xBF", 0) == 0)
        line.erase(0, 3);
    return !line.empty();
}

void runCommandLoop(std::istream& in, std::ostream& out, const Answer& answer)
{
    std::string line;
    while (std::getline(in, line))
    {
        if (normalizeCommandLine(line))
            out << answer(line) << std::endl;
    }
}

CommandBridge::CommandBridge(std::function<void()> wake) : wake_(std::move(wake)) {}

void CommandBridge::attach(Answer answer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    answer_ = std::move(answer);
    changed_.notify_all();
}

void CommandBridge::detach()
{
    std::unique_lock<std::mutex> lock(mutex_);
    answer_ = nullptr;
    changed_.wait(lock, [&] { return inFlight_ == 0; });
}

bool CommandBridge::attached()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<bool>(answer_);
}

std::string CommandBridge::ask(const std::string& line, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!answer_)
    {
        lock.unlock();
        if (wake_)
            wake_();
        lock.lock();
        if (!changed_.wait_for(lock, timeout, [&] { return static_cast<bool>(answer_); }))
            return errorJson("no pilot window opened to answer the command");
    }
    Answer answer = answer_;
    ++inFlight_;
    lock.unlock();
    std::string reply = answer(line);
    lock.lock();
    --inFlight_;
    changed_.notify_all();
    return reply;
}

} // namespace xrpilot
