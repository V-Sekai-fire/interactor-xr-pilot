// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/SpanLog.h"

#include <sstream>

namespace xrpilot
{

void SpanLog::begin(const std::string& id, const std::string& name, const std::string& detail, int64_t nowMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // The innermost running span is the parent.
    std::string parent;
    int depth = 0;
    for (std::deque<Span>::reverse_iterator it = spans_.rbegin(); it != spans_.rend(); ++it)
    {
        if (it->running())
        {
            parent = it->id;
            depth = it->depth + 1;
            break;
        }
    }
    // The last sibling, if it repeats this tool and ended moments ago, takes the repeat.
    for (std::deque<Span>::reverse_iterator it = spans_.rbegin(); it != spans_.rend(); ++it)
    {
        if (it->parent != parent || it->depth != depth)
            continue;
        if (!it->running() && it->name == name && nowMs - it->endMs < SpanDuckMs)
        {
            it->id = id;
            it->detail = detail;
            it->endMs = 0;
            it->ok = true;
            ++it->count;
            return;
        }
        break;
    }
    Span span;
    span.id = id;
    span.name = name;
    span.detail = detail;
    span.startMs = nowMs;
    span.parent = parent;
    span.depth = depth;
    for (std::deque<Span>::iterator it = spans_.begin(); it != spans_.end(); ++it)
    {
        // Every running ancestor counts one more span inside it.
        if (it->running() && it->depth < depth)
            ++it->nested;
    }
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

std::vector<Span> SpanLog::visible() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Span> rows;
    // The latest top-level span stays unfolded, so the most recent motions show without a click.
    std::string latest;
    for (std::deque<Span>::const_reverse_iterator it = spans_.rbegin(); it != spans_.rend(); ++it)
    {
        if (it->parent.empty())
        {
            latest = it->id;
            break;
        }
    }
    // Whether each span shows its nested spans, by id; parents come before their children.
    std::vector<std::pair<std::string, bool>> opens;
    for (const Span& span : spans_)
    {
        bool shown = span.parent.empty();
        if (!shown)
        {
            for (const std::pair<std::string, bool>& open : opens)
            {
                if (open.first == span.parent)
                {
                    shown = open.second;
                    break;
                }
            }
        }
        const bool open = span.running() || (span.id == latest ? !span.expanded : span.expanded);
        opens.emplace_back(span.id, shown && open);
        if (shown)
            rows.push_back(span);
    }
    return rows;
}

void SpanLog::toggle(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (Span& span : spans_)
    {
        if (span.id == id && !span.running())
        {
            span.expanded = !span.expanded;
            return;
        }
    }
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
