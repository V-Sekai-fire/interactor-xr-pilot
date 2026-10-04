// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Every span of the trace kept in SQLite in Essential Tuple Normal Form, as a MaskScore row: the frame
// before the span is its input, the frame after it the edit candidate, and the span's call the
// instruction EditScore scores the pair against. Facts that may be missing (an end, a parent, a detail,
// a score) are satellite relations, never nullable columns.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

namespace xrpilot
{

// A frame of the view: its left eye as RGBA, which the span's metric reads, and the frame as the runtime
// streamed it, which is the only image the store keeps; nothing is re-encoded.
struct TraceFrame
{
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> encoded; // a PyroWave frame, decodable on its own
    int encodedWidth = 0;
    int encodedHeight = 0;
};

class TraceStore
{
public:
    TraceStore() = default;
    ~TraceStore();
    TraceStore(const TraceStore&) = delete;
    TraceStore& operator=(const TraceStore&) = delete;

    // Creates the schema if needed and starts a session at wallClockMs; later times are on the monotonic
    // clock that reads nowMs now, and are kept relative to it. ":memory:" for a store that is not kept.
    bool open(const std::string& path, int64_t wallClockMs, int64_t nowMs, std::string* error);
    void close();
    bool isOpen() const { return db_ != nullptr; }

    // The span key is the MCP server's id; times are milliseconds on one monotonic clock.
    void begin(const std::string& key, const std::string& tool, const std::string& detail, int64_t atMs);
    void end(const std::string& key, bool ok, int64_t atMs);
    // A pilot command, kept in the innermost running span.
    void command(const std::string& text, bool ok, int64_t atMs);

    // Whether a span is waiting for a frame; the window thread then hands one to frame().
    bool wantsFrame();
    // One frame serves every span waiting for one: the input of those that began, the candidate of
    // those that ended. PNG encoding and writes run on the store's own thread.
    void frame(TraceFrame frame);
    // Waits until every frame handed in is written.
    void flush();
    // Frames handed in without stream bytes, and so not kept; their spans stay without a MaskScore row.
    int64_t framesWithoutStream() const { return withoutStream_; }

    int64_t session() const { return session_; }
    sqlite3* db() { return db_; }

private:
    struct Pending
    {
        int64_t spanId = 0;
        bool input = true; // false: the edit candidate
    };
    struct Job
    {
        TraceFrame frame;
        std::vector<Pending> spans;
    };

    bool exec(const char* sql, std::string* error = nullptr);
    int64_t intern(const char* table, const std::string& name);
    void writer();
    void write(const Job& job);

    sqlite3* db_ = nullptr;
    int64_t session_ = 0;
    int64_t epochMs_ = 0;

    std::mutex mutex_;
    std::map<std::string, int64_t> spanByKey_;
    std::vector<int64_t> running_;        // span ids, innermost last
    std::map<int64_t, int> commandSeq_;
    std::map<int64_t, std::vector<uint8_t>> inputThumb_; // a span's input frame, small, for its metric
    std::vector<Pending> wanting_;
    std::deque<Job> jobs_;
    bool busy_ = false;
    int64_t withoutStream_ = 0;
    bool stopping_ = false;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::thread thread_;
};

// The names of every column that holds a NULL in any table of the store; empty when the store keeps
// the rule that a missing fact is a missing row.
std::vector<std::string> nullColumns(sqlite3* db);

} // namespace xrpilot
