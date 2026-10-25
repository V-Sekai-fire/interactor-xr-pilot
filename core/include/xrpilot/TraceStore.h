// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Every span of the trace kept in SQLite in Essential Tuple Normal Form. A span with an input frame is a
// decider example, the shape of chibifire/edscore-decider: its inputs are the view and the pose before it
// and after it (roles view_before, pose_before, view_after, pose_after), its call is the state, and its
// head and hand travel and view change are measures. Choice questions keep a gold answer only where one
// was measured; XR Pilot measures no categorical answer, so it asks none. Facts that may be missing (an
// end, a parent, a detail, a gold answer, a measure, an image's extent) are satellite relations, never
// nullable columns.

#pragma once

#include <atomic>
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

// What the agent saw and where it was at one moment: the whole decoded view and the head and hand poses.
struct TraceFrame
{
    // The whole decoded frame, both eyes side by side (left first), 8-bit sRGB, rows top to bottom.
    // It is kept as lossless JPEG XL: these pixels exactly, tagged sRGB.
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    // The head, then the left and the right hand: position (3) and row-major rotation (9) each, metres.
    std::vector<float> pose;
    // Which frame of the stream this is; a span's end only takes a frame newer than its input.
    uint64_t sequence = 0;
};

// Floats in a TraceFrame's pose.
constexpr size_t TracePoseFloats = 36;

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
    // A span that starts and ends at once and takes no frame: one UI action (see UiSpans.h).
    void mark(const std::string& key, const std::string& tool, const std::string& detail, bool ok, int64_t atMs);
    // A pilot command, kept in the innermost running span.
    void command(const std::string& text, bool ok, int64_t atMs);

    // Whether a span is waiting for a frame; the window thread then hands one to frame().
    bool wantsFrame();
    // One frame serves every span waiting for one: the input of those that began, the candidate of
    // those that ended. PNG encoding and writes run on the store's own thread.
    void frame(TraceFrame frame);
    // Waits until every frame handed in is written.
    void flush();
    // Frames handed in without whole pixels, or that did not encode, and so not kept; their spans stay
    // without a decider example.
    int64_t framesNotKept() const { return notKept_; }

    int64_t session() const { return session_; }
    sqlite3* db() { return db_; }

private:
    struct Pending
    {
        uint64_t after = 0; // an end waits for a frame with a sequence above this
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
    std::map<int64_t, std::vector<uint8_t>> inputThumb_; // a span's input view, small, for its metric
    std::map<int64_t, std::vector<float>> inputPose_;     // a span's input pose, for its metric
    std::map<int64_t, uint64_t> inputSequence_;           // the frame a span's input was taken from
    std::vector<Pending> wanting_;
    std::deque<Job> jobs_;
    bool busy_ = false;
    std::atomic<int64_t> notKept_{0};
    bool stopping_ = false;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::thread thread_;
};

// RECTGTN's times: an ISO 8601 duration (PT1M2.5S) for an offset from an origin, and civil time, ISO 8601
// UTC (2026-10-04T17:02:13.123Z), for the origin itself.
std::string isoDuration(int64_t ms);
std::string civilTime(int64_t unixMs);

// The names of every column that holds a NULL in any table of the store; empty when the store keeps
// the rule that a missing fact is a missing row.
std::vector<std::string> nullColumns(sqlite3* db);

} // namespace xrpilot
