// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/TraceStore.h"

#include "lib/jxl/enc_fast_lossless.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace xrpilot
{

namespace
{

// The interned vocabularies, one relation each.
const char* const Vocabularies[] = {"tool", "task_type", "input_role", "asset_kind", "metric"};

// Bumped when a relation changes; an older store is moved aside, not altered.
constexpr int SchemaVersion = 5;

const char* const Schema = R"sql(
CREATE TABLE IF NOT EXISTS session (
    session_id INTEGER PRIMARY KEY,
    uuid TEXT NOT NULL UNIQUE,  -- UUIDv7 (RFC 9562) at the session's start
    started_at TEXT NOT NULL);  -- civil time, ISO 8601 UTC: the origin every span time counts from
CREATE TABLE IF NOT EXISTS span (
    span_id INTEGER PRIMARY KEY,
    uuid TEXT NOT NULL UNIQUE,  -- UUIDv7 at the span's start
    session_id INTEGER NOT NULL REFERENCES session,
    span_key TEXT NOT NULL,
    tool_id INTEGER NOT NULL REFERENCES tool,
    started TEXT NOT NULL);     -- ISO 8601 duration from the session's origin, as RECTGTN's start
CREATE TABLE IF NOT EXISTS span_detail (
    span_id INTEGER PRIMARY KEY REFERENCES span,
    detail TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS span_parent (
    span_id INTEGER PRIMARY KEY REFERENCES span,
    parent_span_id INTEGER NOT NULL REFERENCES span);
CREATE TABLE IF NOT EXISTS span_end (
    span_id INTEGER PRIMARY KEY REFERENCES span,
    ended TEXT NOT NULL,
    ok INTEGER NOT NULL CHECK (ok IN (0, 1)));
CREATE TABLE IF NOT EXISTS span_command (
    span_id INTEGER NOT NULL REFERENCES span,
    seq INTEGER NOT NULL,
    at TEXT NOT NULL,
    command TEXT NOT NULL,
    ok INTEGER NOT NULL CHECK (ok IN (0, 1)),
    PRIMARY KEY (span_id, seq));
CREATE TABLE IF NOT EXISTS asset (
    asset_id INTEGER PRIMARY KEY,
    uuid TEXT NOT NULL UNIQUE,  -- UUIDv7 at the moment the frame reached the store
    asset_kind_id INTEGER NOT NULL REFERENCES asset_kind,
    bytes BLOB NOT NULL);
-- Only images have an extent.
CREATE TABLE IF NOT EXISTS asset_extent (
    asset_id INTEGER PRIMARY KEY REFERENCES asset,
    width INTEGER NOT NULL,
    height INTEGER NOT NULL);
-- A span with an input frame is a decider example (chibifire/edscore-decider's shape): the frame and
-- pose before the span and after it, typed Choice questions, a gold answer only where one was
-- measured, and real-valued measures kept as measures. XR Pilot measures travel and change, so it
-- writes measures and asks no question; cua-tracer asks the effect question. Same schema in both.
CREATE TABLE IF NOT EXISTS question (
    question_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    text TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS question_option (
    question_id INTEGER NOT NULL REFERENCES question,
    option_index INTEGER NOT NULL,
    label TEXT NOT NULL,
    PRIMARY KEY (question_id, option_index),
    UNIQUE (question_id, label));
CREATE TABLE IF NOT EXISTS decider_example (
    span_id INTEGER PRIMARY KEY REFERENCES span,
    task_type_id INTEGER NOT NULL REFERENCES task_type);
CREATE TABLE IF NOT EXISTS decider_input (
    span_id INTEGER NOT NULL REFERENCES decider_example,
    input_role_id INTEGER NOT NULL REFERENCES input_role,
    asset_id INTEGER NOT NULL REFERENCES asset,
    PRIMARY KEY (span_id, input_role_id));
CREATE TABLE IF NOT EXISTS decider_question (
    span_id INTEGER NOT NULL REFERENCES decider_example,
    question_id INTEGER NOT NULL REFERENCES question,
    PRIMARY KEY (span_id, question_id));
CREATE TABLE IF NOT EXISTS decider_gold (
    span_id INTEGER NOT NULL,
    question_id INTEGER NOT NULL,
    option_index INTEGER NOT NULL,
    PRIMARY KEY (span_id, question_id),
    FOREIGN KEY (span_id, question_id) REFERENCES decider_question,
    FOREIGN KEY (question_id, option_index) REFERENCES question_option);
CREATE TABLE IF NOT EXISTS decider_measure (
    span_id INTEGER NOT NULL REFERENCES decider_example,
    metric_id INTEGER NOT NULL REFERENCES metric,
    value REAL NOT NULL,
    PRIMARY KEY (span_id, metric_id));
-- One row per example and question: the state, the question, its options in order, and the gold
-- answer's label where one was measured (gold is NULL here only because the view outer-joins).
CREATE VIEW IF NOT EXISTS decider_row AS
SELECT e.span_id, s.session_id, tt.name AS task_type,
       t.name || COALESCE(' ' || d.detail, '') AS state,
       q.name AS question, q.text AS question_text,
       (SELECT json_group_array(label) FROM
           (SELECT label FROM question_option o WHERE o.question_id = q.question_id ORDER BY option_index))
           AS options,
       go.label AS gold
FROM decider_example e
JOIN span s USING (span_id)
JOIN tool t USING (tool_id)
JOIN task_type tt USING (task_type_id)
LEFT JOIN span_detail d USING (span_id)
JOIN decider_question dq ON dq.span_id = e.span_id
JOIN question q ON q.question_id = dq.question_id
LEFT JOIN decider_gold g ON g.span_id = dq.span_id AND g.question_id = dq.question_id
LEFT JOIN question_option go ON go.question_id = g.question_id AND go.option_index = g.option_index;
)sql";

class Statement
{
public:
    Statement(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr); }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int i, int64_t v)
    {
        sqlite3_bind_int64(stmt_, i, v);
        return *this;
    }
    Statement& bind(int i, double v)
    {
        sqlite3_bind_double(stmt_, i, v);
        return *this;
    }
    Statement& bind(int i, const std::string& v)
    {
        sqlite3_bind_text(stmt_, i, v.c_str(), int(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& blob(int i, const std::vector<uint8_t>& v)
    {
        sqlite3_bind_blob(stmt_, i, v.data(), int(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    bool step() { return stmt_ != nullptr && sqlite3_step(stmt_) == SQLITE_ROW; }
    bool run() { return stmt_ != nullptr && sqlite3_step(stmt_) == SQLITE_DONE; }
    int64_t column(int i) { return sqlite3_column_int64(stmt_, i); }
    std::string text(int i)
    {
        const unsigned char* t = sqlite3_column_text(stmt_, i);
        return t != nullptr ? reinterpret_cast<const char*>(t) : "";
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

// A UUIDv7 (RFC 9562, section 5.7) whose time is unixMs: 48 bits of milliseconds, version 7, 74
// random bits, variant 10. Every session, span and asset is named by one, so rows from many stores
// merge without collisions.
std::string uuid7(int64_t unixMs)
{
    static std::mutex lock;
    static std::random_device device;
    uint64_t random[2];
    {
        std::lock_guard<std::mutex> guard(lock);
        random[0] = (uint64_t(device()) << 32) | device();
        random[1] = (uint64_t(device()) << 32) | device();
    }
    uint8_t b[16];
    const uint64_t ms = uint64_t(std::max<int64_t>(unixMs, 0));
    for (int i = 0; i < 6; ++i)
        b[i] = uint8_t(ms >> (40 - 8 * i));
    for (int i = 0; i < 2; ++i)
        b[6 + i] = uint8_t(random[0] >> (8 * i));
    for (int i = 0; i < 8; ++i)
        b[8 + i] = uint8_t(random[1] >> (8 * i));
    b[6] = uint8_t(0x70 | (b[6] & 0x0f));
    b[8] = uint8_t(0x80 | (b[8] & 0x3f));
    static const char* const hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out += '-';
        out += hex[b[i] >> 4];
        out += hex[b[i] & 0x0f];
    }
    return out;
}

// The frame as lossless JPEG XL (ISO/IEC 18181): a bare codestream of its RGB, tagged sRGB, from libjxl's
// fast lossless encoder at effort 1, the speed a 16-frame-a-second trace keeps up with on one thread.
// Empty when it does not encode.
std::vector<uint8_t> jxlLossless(const TraceFrame& f)
{
    const size_t pixels = size_t(f.width) * size_t(f.height);
    std::vector<uint8_t> rgb(pixels * 3);
    for (size_t i = 0; i < pixels; ++i)
        std::memcpy(&rgb[i * 3], &f.rgba[i * 4], 3);
    unsigned char* out = nullptr;
    const size_t size = JxlFastLosslessEncode(rgb.data(), size_t(f.width), size_t(f.width) * 3, size_t(f.height), 3, 8,
                                              0, 1, &out, nullptr, nullptr);
    std::vector<uint8_t> bytes;
    if (out != nullptr && size > 0)
        bytes.assign(out, out + size);
    std::free(out);
    return bytes;
}

// One gray value per 8x8 block: what a span's metric compares.
std::vector<uint8_t> thumb(const TraceFrame& f)
{
    std::vector<uint8_t> out;
    for (int y = 0; y + 8 <= f.height; y += 8)
    {
        for (int x = 0; x + 8 <= f.width; x += 8)
        {
            const uint8_t* p = &f.rgba[(size_t(y) * size_t(f.width) + size_t(x)) * 4];
            out.push_back(uint8_t((int(p[0]) + 2 * int(p[1]) + int(p[2])) / 4));
        }
    }
    return out;
}

} // namespace

std::string isoDuration(int64_t ms)
{
    ms = std::max<int64_t>(0, ms);
    const int64_t hours = ms / 3'600'000;
    const int64_t minutes = ms / 60'000 % 60;
    const int64_t seconds = ms / 1000 % 60;
    const int64_t millis = ms % 1000;
    std::string out = "PT";
    if (hours > 0)
        out += std::to_string(hours) + "H";
    if (minutes > 0)
        out += std::to_string(minutes) + "M";
    if (seconds > 0 || millis > 0 || out == "PT")
    {
        out += std::to_string(seconds);
        if (millis > 0)
        {
            char frac[8];
            std::snprintf(frac, sizeof(frac), ".%03lld", (long long)millis);
            std::string f = frac;
            while (f.back() == '0')
                f.pop_back();
            out += f;
        }
        out += "S";
    }
    return out;
}

std::string civilTime(int64_t unixMs)
{
    const time_t seconds = time_t(unixMs / 1000);
    std::tm utc = {};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char out[40];
    std::snprintf(out, sizeof(out), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, (long long)(unixMs % 1000));
    return out;
}

TraceStore::~TraceStore()
{
    close();
}

bool TraceStore::exec(const char* sql, std::string* error)
{
    char* message = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) == SQLITE_OK)
        return true;
    if (error != nullptr)
        *error = message != nullptr ? message : "sqlite error";
    sqlite3_free(message);
    return false;
}

bool TraceStore::open(const std::string& path, int64_t wallClockMs, int64_t nowMs, std::string* error)
{
    close();
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
    {
        if (error != nullptr)
            *error = db_ != nullptr ? sqlite3_errmsg(db_) : "cannot open " + path;
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);
    int64_t have = 0;
    int64_t used = 0;
    {
        Statement version(db_, "PRAGMA user_version");
        Statement tables(db_, "SELECT count(*) FROM sqlite_master WHERE type = 'table'");
        have = version.step() ? version.column(0) : 0;
        used = tables.step() ? tables.column(0) : 0;
    }
    {
        if (used > 0 && have != SchemaVersion)
        {
            // An older store keeps its rows under its own name; this one starts over.
            sqlite3_close(db_);
            db_ = nullptr;
            const std::string aside = path + ".v" + std::to_string(have);
            if (std::rename(path.c_str(), aside.c_str()) != 0)
            {
                if (error != nullptr)
                    *error = "cannot move schema v" + std::to_string(have) + " store aside to " + aside;
                return false;
            }
            std::remove((path + "-wal").c_str());
            std::remove((path + "-shm").c_str());
            return open(path, wallClockMs, nowMs, error);
        }
    }
    bool ok = exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;", error);
    for (const char* table : Vocabularies)
    {
        const std::string sql = std::string("CREATE TABLE IF NOT EXISTS ") + table + " (" + table +
                                "_id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);";
        ok = ok && exec(sql.c_str(), error);
    }
    ok = ok && exec(Schema, error);
    ok = ok && exec(("PRAGMA user_version = " + std::to_string(SchemaVersion)).c_str(), error);
    if (ok)
    {
        Statement insert(db_, "INSERT INTO session (uuid, started_at) VALUES (?, ?)");
        ok = insert.bind(1, uuid7(wallClockMs)).bind(2, civilTime(wallClockMs)).run();
        session_ = sqlite3_last_insert_rowid(db_);
    }
    if (!ok)
    {
        if (error != nullptr && error->empty())
            *error = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    epochMs_ = nowMs;
    wallMs_ = wallClockMs;
    stopping_ = false;
    thread_ = std::thread([this] { writer(); });
    return true;
}

void TraceStore::close()
{
    if (db_ == nullptr)
        return;
    flush();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
    // The tray marks UI spans from its own threads; they find the store closed, not half closed.
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_close(db_);
    db_ = nullptr;
    spanByKey_.clear();
    running_.clear();
    commandSeq_.clear();
    inputThumb_.clear();
    inputSequence_.clear();
    wanting_.clear();
}

int64_t TraceStore::intern(const char* table, const std::string& name)
{
    const std::string insert = std::string("INSERT OR IGNORE INTO ") + table + " (name) VALUES (?)";
    Statement(db_, insert.c_str()).bind(1, name).run();
    const std::string select = std::string("SELECT ") + table + "_id FROM " + table + " WHERE name = ?";
    Statement find(db_, select.c_str());
    find.bind(1, name);
    return find.step() ? find.column(0) : 0;
}

void TraceStore::begin(const std::string& key, const std::string& tool, const std::string& detail, int64_t atMs)
{
    if (db_ == nullptr)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    Statement insert(db_, "INSERT INTO span (uuid, session_id, span_key, tool_id, started) VALUES (?, ?, ?, ?, ?)");
    insert.bind(1, uuid7(wallMs_ + atMs - epochMs_))
        .bind(2, session_)
        .bind(3, key)
        .bind(4, intern("tool", tool))
        .bind(5, isoDuration(atMs - epochMs_))
        .run();
    const int64_t id = sqlite3_last_insert_rowid(db_);
    if (!detail.empty())
        Statement(db_, "INSERT INTO span_detail VALUES (?, ?)").bind(1, id).bind(2, detail).run();
    if (!running_.empty())
        Statement(db_, "INSERT INTO span_parent VALUES (?, ?)").bind(1, id).bind(2, running_.back()).run();
    spanByKey_[key] = id;
    running_.push_back(id);
    wanting_.push_back({0, id, true});
}

void TraceStore::mark(const std::string& key, const std::string& tool, const std::string& detail, bool ok, int64_t atMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr)
        return;
    const std::string at = isoDuration(atMs - epochMs_);
    Statement insert(db_, "INSERT INTO span (uuid, session_id, span_key, tool_id, started) VALUES (?, ?, ?, ?, ?)");
    insert.bind(1, uuid7(wallMs_ + atMs - epochMs_)).bind(2, session_).bind(3, key).bind(4, intern("tool", tool)).bind(5, at).run();
    const int64_t id = sqlite3_last_insert_rowid(db_);
    if (!detail.empty())
        Statement(db_, "INSERT INTO span_detail VALUES (?, ?)").bind(1, id).bind(2, detail).run();
    if (!running_.empty())
        Statement(db_, "INSERT INTO span_parent VALUES (?, ?)").bind(1, id).bind(2, running_.back()).run();
    Statement(db_, "INSERT INTO span_end VALUES (?, ?, ?)").bind(1, id).bind(2, at).bind(3, int64_t(ok ? 1 : 0)).run();
}

void TraceStore::end(const std::string& key, bool ok, int64_t atMs)
{
    if (db_ == nullptr)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::map<std::string, int64_t>::iterator found = spanByKey_.find(key);
    if (found == spanByKey_.end())
        return;
    const int64_t id = found->second;
    Statement(db_, "INSERT OR REPLACE INTO span_end VALUES (?, ?, ?)")
        .bind(1, id)
        .bind(2, isoDuration(atMs - epochMs_))
        .bind(3, int64_t(ok ? 1 : 0))
        .run();
    running_.erase(std::remove(running_.begin(), running_.end(), id), running_.end());
    const std::map<int64_t, uint64_t>::iterator taken = inputSequence_.find(id);
    // An end whose input frame is not taken yet waits for any frame after it.
    wanting_.push_back({taken != inputSequence_.end() ? taken->second : UINT64_MAX, id, false});
}

void TraceStore::command(const std::string& text, bool ok, int64_t atMs)
{
    if (db_ == nullptr)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.empty())
        return;
    const int64_t id = running_.back();
    Statement(db_, "INSERT INTO span_command VALUES (?, ?, ?, ?, ?)")
        .bind(1, id)
        .bind(2, int64_t(commandSeq_[id]++))
        .bind(3, isoDuration(atMs - epochMs_))
        .bind(4, text)
        .bind(5, int64_t(ok ? 1 : 0))
        .run();
}

bool TraceStore::wantsFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !wanting_.empty();
}

void TraceStore::frame(TraceFrame frame)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wanting_.empty() || db_ == nullptr || frame.width <= 0 || frame.height <= 0)
            return;
        if (frame.rgba.size() != size_t(frame.width) * size_t(frame.height) * 4)
        {
            ++notKept_;
            return;
        }
        Job job;
        std::vector<Pending> later;
        for (Pending& p : wanting_)
        {
            if (p.input)
            {
                inputSequence_[p.spanId] = frame.sequence;
                job.spans.push_back(p);
            }
            else if (p.after != UINT64_MAX && frame.sequence > p.after)
            {
                inputSequence_.erase(p.spanId);
                job.spans.push_back(p);
            }
            else
            {
                // A span that began and ended within one frame waits for the next.
                if (p.after == UINT64_MAX)
                {
                    const std::map<int64_t, uint64_t>::iterator taken = inputSequence_.find(p.spanId);
                    if (taken != inputSequence_.end())
                        p.after = taken->second;
                }
                later.push_back(p);
            }
        }
        wanting_.swap(later);
        if (job.spans.empty())
            return;
        job.frame = std::move(frame);
        job.wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
        jobs_.push_back(std::move(job));
    }
    wake_.notify_all();
}

void TraceStore::flush()
{
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

void TraceStore::writer()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (true)
    {
        wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (jobs_.empty() && stopping_)
            return;
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        busy_ = true;
        lock.unlock();
        write(job);
        lock.lock();
        busy_ = false;
        idle_.notify_all();
    }
}

void TraceStore::write(const Job& job)
{
    const TraceFrame& f = job.frame;
    const std::vector<uint8_t> small = thumb(f);
    const bool posed = f.pose.size() == TracePoseFloats;
    const std::vector<uint8_t> encoded = jxlLossless(f);
    if (encoded.empty())
    {
        ++notKept_;
        return;
    }
    exec("BEGIN");
    Statement(db_, "INSERT INTO asset (uuid, asset_kind_id, bytes) VALUES (?, ?, ?)")
        .bind(1, uuid7(job.wallMs))
        .bind(2, intern("asset_kind", "jxl"))
        .blob(3, encoded)
        .run();
    const int64_t view = sqlite3_last_insert_rowid(db_);
    Statement(db_, "INSERT INTO asset_extent VALUES (?, ?, ?)")
        .bind(1, view)
        .bind(2, int64_t(f.width))
        .bind(3, int64_t(f.height))
        .run();
    int64_t pose = 0;
    if (posed)
    {
        std::vector<uint8_t> bytes(f.pose.size() * sizeof(float));
        std::memcpy(bytes.data(), f.pose.data(), bytes.size());
        Statement(db_, "INSERT INTO asset (uuid, asset_kind_id, bytes) VALUES (?, ?, ?)")
            .bind(1, uuid7(job.wallMs))
            .bind(2, intern("asset_kind", "pose_f32x36"))
            .blob(3, bytes)
            .run();
        pose = sqlite3_last_insert_rowid(db_);
    }
    const int64_t viewBefore = intern("input_role", "view_before");
    const int64_t poseBefore = intern("input_role", "pose_before");
    const int64_t viewAfter = intern("input_role", "view_after");
    const int64_t poseAfter = intern("input_role", "pose_after");
    for (const Pending& p : job.spans)
    {
        if (p.input)
        {
            Statement(db_, "INSERT OR IGNORE INTO decider_example VALUES (?, ?)")
                .bind(1, p.spanId)
                .bind(2, intern("task_type", "agent_step"))
                .run();
            Statement(db_, "INSERT OR IGNORE INTO decider_input VALUES (?, ?, ?)")
                .bind(1, p.spanId)
                .bind(2, viewBefore)
                .bind(3, view)
                .run();
            if (posed)
                Statement(db_, "INSERT OR IGNORE INTO decider_input VALUES (?, ?, ?)")
                    .bind(1, p.spanId)
                    .bind(2, poseBefore)
                    .bind(3, pose)
                    .run();
            std::lock_guard<std::mutex> lock(mutex_);
            inputThumb_[p.spanId] = small;
            if (posed)
                inputPose_[p.spanId] = f.pose;
            continue;
        }
        // A span whose inputs were never taken is no example, and so has no after.
        const char* const insertAfter = "INSERT OR IGNORE INTO decider_input SELECT ?, ?, ? "
                                        "WHERE EXISTS (SELECT 1 FROM decider_example WHERE span_id = ?)";
        Statement(db_, insertAfter).bind(1, p.spanId).bind(2, viewAfter).bind(3, view).bind(4, p.spanId).run();
        if (posed)
            Statement(db_, insertAfter).bind(1, p.spanId).bind(2, poseAfter).bind(3, pose).bind(4, p.spanId).run();
        std::vector<uint8_t> before;
        std::vector<float> posedBefore;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::map<int64_t, std::vector<uint8_t>>::iterator found = inputThumb_.find(p.spanId);
            if (found != inputThumb_.end())
            {
                before.swap(found->second);
                inputThumb_.erase(found);
            }
            const std::map<int64_t, std::vector<float>>::iterator foundPose = inputPose_.find(p.spanId);
            if (foundPose != inputPose_.end())
            {
                posedBefore.swap(foundPose->second);
                inputPose_.erase(foundPose);
            }
        }
        // A measure needs the span's after of that modality.
        const char* const insertMeasure = "INSERT OR REPLACE INTO decider_measure SELECT ?, ?, ? WHERE EXISTS "
                                          "(SELECT 1 FROM decider_input WHERE span_id = ? AND input_role_id = ?)";
        if (!before.empty() && before.size() == small.size())
        {
            double sum = 0.0;
            for (size_t i = 0; i < small.size(); ++i)
                sum += std::abs(int(small[i]) - int(before[i]));
            Statement(db_, insertMeasure)
                .bind(1, p.spanId)
                .bind(2, intern("metric", "gray_l1_8x8"))
                .bind(3, sum / double(small.size()) / 255.0)
                .bind(4, p.spanId)
                .bind(5, viewAfter)
                .run();
        }
        if (posed && posedBefore.size() == TracePoseFloats)
        {
            // How far the head and each hand travelled over the span, metres.
            const char* const names[3] = {"head_travel_m", "left_hand_travel_m", "right_hand_travel_m"};
            for (int part = 0; part < 3; ++part)
            {
                const float* a = &posedBefore[size_t(part) * 12];
                const float* b = &f.pose[size_t(part) * 12];
                const double d = std::sqrt(double((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) +
                                                  (b[2] - a[2]) * (b[2] - a[2])));
                Statement(db_, insertMeasure)
                    .bind(1, p.spanId)
                    .bind(2, intern("metric", names[part]))
                    .bind(3, d)
                    .bind(4, p.spanId)
                    .bind(5, poseAfter)
                    .run();
            }
        }
    }
    exec("COMMIT");
}

std::vector<std::string> nullColumns(sqlite3* db)
{
    std::vector<std::string> out;
    std::vector<std::string> tables;
    {
        Statement list(db, "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'");
        while (list.step())
            tables.push_back(list.text(0));
    }
    for (const std::string& table : tables)
    {
        std::vector<std::string> columns;
        {
            Statement info(db, ("PRAGMA table_info(" + table + ")").c_str());
            while (info.step())
                columns.push_back(info.text(1));
        }
        for (const std::string& column : columns)
        {
            Statement count(db, ("SELECT count(*) FROM " + table + " WHERE " + column + " IS NULL").c_str());
            if (count.step() && count.column(0) > 0)
                out.push_back(table + "." + column);
        }
    }
    return out;
}

} // namespace xrpilot
