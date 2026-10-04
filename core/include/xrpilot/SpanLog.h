// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The commands as a trace: each MCP tool call is a span with a start, an end and a status, and the
// pilot commands it sent while open are its children, as OpenTelemetry shows a request.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace xrpilot
{

struct SpanChild
{
    std::string text;
    int64_t atMs = 0;
    bool ok = true;
};

struct Span
{
    std::string id;
    std::string name;   // the tool
    std::string detail; // its arguments
    int64_t startMs = 0;
    int64_t endMs = 0; // 0 while running
    bool ok = true;
    int count = 1; // repeats ducked into this span
    std::vector<SpanChild> children;
    std::string parent; // the span open when this one began; empty at the top
    int depth = 0;
    int nested = 0;        // spans begun inside this one, at any depth
    bool expanded = false; // a finished span shows its nested spans only when expanded

    bool running() const { return endMs == 0; }
    int64_t durationMs(int64_t nowMs) const { return (running() ? nowMs : endMs) - startMs; }
};

// How close together repeats of one tool are ducked into a single span.
constexpr int64_t SpanDuckMs = 2000;
// A span keeps at most this many children, the latest.
constexpr size_t SpanChildLimit = 8;

class SpanLog
{
public:
    explicit SpanLog(size_t capacity = 50) : capacity_(capacity) {}
    // Safe from any thread. A span begun while another runs nests inside it. A begin that repeats its
    // last sibling's tool within SpanDuckMs of that sibling's end reopens it and counts it instead.
    void begin(const std::string& id, const std::string& name, const std::string& detail, int64_t nowMs);
    // Ends the span with this id; an unknown id changes nothing.
    void end(const std::string& id, bool ok, int64_t nowMs);
    // A pilot command, attached to the open span, or dropped when none is open.
    void child(const std::string& text, bool ok, int64_t nowMs);
    // Oldest first, every span.
    std::vector<Span> recent() const;
    // The rows a trace shows, oldest first: a nested span shows while every span above it is
    // running or expanded, so a finished span folds what ran inside it.
    std::vector<Span> visible() const;
    // Expands a finished span, or folds it again.
    void toggle(const std::string& id);

private:
    mutable std::mutex mutex_;
    std::deque<Span> spans_;
    size_t capacity_;
};

// A "span begin <id> <name> [detail]" or "span end <id> <ok|error>" line; false for anything else.
struct SpanLine
{
    bool begin = false;
    std::string id;
    std::string name;   // begin
    std::string detail; // begin
    bool ok = true;     // end
};
bool parseSpanLine(const std::string& line, SpanLine& out);

} // namespace xrpilot
