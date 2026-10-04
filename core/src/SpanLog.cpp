// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/SpanLog.h"

#include <sstream>

namespace xrpilot
{

void SpanLog::begin(const std::string& id, const std::string& name, const std::string& detail, int64_t nowMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!spans_.empty())
    {
        Span& last = spans_.back();
        if (!last.running() && last.name == name && nowMs - last.endMs < SpanDuckMs)
        {
            last.id = id;
            last.detail = detail;
            last.endMs = 0;
            last.ok = true;
            ++last.count;
            return;
        }
    }
    Span span;
    span.id = id;
    span.name = name;
    span.detail = detail;
    span.startMs = nowMs;
    spans_.push_back(std::move(span));
    while (spans_.size() > capacity_)
        spans_.pop_front();
}

void SpanLog::end(const std::string& id, bool ok, int64_t nowMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::deque<Span>::reverse_iterator it = spans_.rbegin(); it != spans_.rend(); ++it)
    {
        if (it->id != id || !it->running())
            continue;
        it->endMs = nowMs > it->startMs ? nowMs : it->startMs + 1;
        it->ok = it->ok && ok;
        return;
    }
}

void SpanLog::child(const std::string& text, bool ok, int64_t nowMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (spans_.empty() || !spans_.back().running())
        return;
    Span& open = spans_.back();
    open.children.push_back(SpanChild{text, nowMs, ok});
    if (!ok)
        open.ok = false;
    if (open.children.size() > SpanChildLimit)
        open.children.erase(open.children.begin());
}

std::vector<Span> SpanLog::recent() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<Span>(spans_.begin(), spans_.end());
}

bool parseSpanLine(const std::string& line, SpanLine& out)
{
    out = SpanLine{};
    std::istringstream in(line);
    std::string verb;
    std::string kind;
    in >> verb >> kind >> out.id;
    if (verb != "span" || out.id.empty())
        return false;
    if (kind == "begin")
    {
        if (!(in >> out.name))
            return false;
        std::getline(in >> std::ws, out.detail);
        out.begin = true;
        return true;
    }
    if (kind == "end")
    {
        std::string status;
        in >> status;
        if (status != "ok" && status != "error")
            return false;
        out.begin = false;
        out.ok = status == "ok";
        return true;
    }
    return false;
}

} // namespace xrpilot
