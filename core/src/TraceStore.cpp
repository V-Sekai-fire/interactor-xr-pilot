// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/TraceStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace xrpilot
{

namespace
{

// The interned vocabularies, one relation each.
const char* const Vocabularies[] = {"tool",           "task_type",      "dimension", "input_column",
                                    "asset_kind",     "candidate_axis", "candidate_kind", "metric"};

const char* const Schema = R"sql(
CREATE TABLE IF NOT EXISTS session (
    session_id INTEGER PRIMARY KEY,
    started_unix_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS span (
    span_id INTEGER PRIMARY KEY,
    session_id INTEGER NOT NULL REFERENCES session,
    span_key TEXT NOT NULL,
    tool_id INTEGER NOT NULL REFERENCES tool,
    started_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS span_detail (
    span_id INTEGER PRIMARY KEY REFERENCES span,
    detail TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS span_parent (
    span_id INTEGER PRIMARY KEY REFERENCES span,
    parent_span_id INTEGER NOT NULL REFERENCES span);
CREATE TABLE IF NOT EXISTS span_end (
    span_id INTEGER PRIMARY KEY REFERENCES span,
    ended_ms INTEGER NOT NULL,
    ok INTEGER NOT NULL CHECK (ok IN (0, 1)));
CREATE TABLE IF NOT EXISTS span_command (
    span_id INTEGER NOT NULL REFERENCES span,
    seq INTEGER NOT NULL,
    at_ms INTEGER NOT NULL,
    command TEXT NOT NULL,
    ok INTEGER NOT NULL CHECK (ok IN (0, 1)),
    PRIMARY KEY (span_id, seq));
CREATE TABLE IF NOT EXISTS asset (
    asset_id INTEGER PRIMARY KEY,
    asset_kind_id INTEGER NOT NULL REFERENCES asset_kind,
    width INTEGER NOT NULL,
    height INTEGER NOT NULL,
    bytes BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS maskscore_root (
    span_id INTEGER PRIMARY KEY REFERENCES span,
    task_type_id INTEGER NOT NULL REFERENCES task_type,
    dimension_id INTEGER NOT NULL REFERENCES dimension,
    input_column_id INTEGER NOT NULL REFERENCES input_column,
    input_asset_id INTEGER NOT NULL REFERENCES asset);
CREATE TABLE IF NOT EXISTS maskscore_candidate (
    span_id INTEGER NOT NULL REFERENCES maskscore_root,
    candidate_axis_id INTEGER NOT NULL REFERENCES candidate_axis,
    rank INTEGER NOT NULL,
    candidate_asset_id INTEGER NOT NULL REFERENCES asset,
    candidate_kind_id INTEGER NOT NULL REFERENCES candidate_kind,
    PRIMARY KEY (span_id, candidate_axis_id, rank));
CREATE TABLE IF NOT EXISTS maskscore_score (
    span_id INTEGER NOT NULL,
    candidate_axis_id INTEGER NOT NULL,
    rank INTEGER NOT NULL,
    metric_id INTEGER NOT NULL REFERENCES metric,
    metric_value REAL NOT NULL,
    PRIMARY KEY (span_id, candidate_axis_id, rank, metric_id),
    FOREIGN KEY (span_id, candidate_axis_id, rank) REFERENCES maskscore_candidate);
-- EditScore's triple, for a scorer to read: the instruction is the span's tool and its arguments. A
-- pyrowave image is a frame as streamed (both eyes, left first) that the scorer decodes.
CREATE VIEW IF NOT EXISTS editscore_pair AS
SELECT r.span_id, s.session_id, sk.name AS source_kind, src.bytes AS source, ek.name AS edited_kind,
       ed.bytes AS edited, t.name || COALESCE(' ' || d.detail, '') AS instruction
FROM maskscore_root r
JOIN span s USING (span_id)
JOIN tool t USING (tool_id)
LEFT JOIN span_detail d USING (span_id)
JOIN asset src ON src.asset_id = r.input_asset_id
JOIN asset_kind sk ON sk.asset_kind_id = src.asset_kind_id
JOIN maskscore_candidate c ON c.span_id = r.span_id AND c.rank = 1
JOIN asset ed ON ed.asset_id = c.candidate_asset_id
JOIN asset_kind ek ON ek.asset_kind_id = ed.asset_kind_id;
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
    bool ok = exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;", error);
    for (const char* table : Vocabularies)
    {
        const std::string sql = std::string("CREATE TABLE IF NOT EXISTS ") + table + " (" + table +
                                "_id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);";
        ok = ok && exec(sql.c_str(), error);
    }
    ok = ok && exec(Schema, error);
    if (ok)
    {
        Statement insert(db_, "INSERT INTO session (started_unix_ms) VALUES (?)");
        ok = insert.bind(1, wallClockMs).run();
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
    sqlite3_close(db_);
    db_ = nullptr;
    spanByKey_.clear();
    running_.clear();
    commandSeq_.clear();
    inputThumb_.clear();
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
    Statement insert(db_, "INSERT INTO span (session_id, span_key, tool_id, started_ms) VALUES (?, ?, ?, ?)");
    insert.bind(1, session_).bind(2, key).bind(3, intern("tool", tool)).bind(4, atMs - epochMs_).run();
    const int64_t id = sqlite3_last_insert_rowid(db_);
    if (!detail.empty())
        Statement(db_, "INSERT INTO span_detail VALUES (?, ?)").bind(1, id).bind(2, detail).run();
    if (!running_.empty())
        Statement(db_, "INSERT INTO span_parent VALUES (?, ?)").bind(1, id).bind(2, running_.back()).run();
    spanByKey_[key] = id;
    running_.push_back(id);
    wanting_.push_back({id, true});
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
        .bind(2, atMs - epochMs_)
        .bind(3, int64_t(ok ? 1 : 0))
        .run();
    running_.erase(std::remove(running_.begin(), running_.end(), id), running_.end());
    wanting_.push_back({id, false});
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
        .bind(3, atMs - epochMs_)
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
        if (frame.encoded.empty())
        {
            ++withoutStream_;
            return;
        }
        Job job;
        job.frame = std::move(frame);
        job.spans.swap(wanting_);
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
    exec("BEGIN");
    Statement(db_, "INSERT INTO asset (asset_kind_id, width, height, bytes) VALUES (?, ?, ?, ?)")
        .bind(1, intern("asset_kind", "pyrowave"))
        .bind(2, int64_t(f.encodedWidth))
        .bind(3, int64_t(f.encodedHeight))
        .blob(4, f.encoded)
        .run();
    const int64_t asset = sqlite3_last_insert_rowid(db_);
    const int64_t edit = intern("candidate_axis", "edit");
    for (const Pending& p : job.spans)
    {
        if (p.input)
        {
            Statement(db_, "INSERT OR IGNORE INTO maskscore_root VALUES (?, ?, ?, ?, ?)")
                .bind(1, p.spanId)
                .bind(2, intern("task_type", "agent_step"))
                .bind(3, intern("dimension", "instruction_following"))
                .bind(4, intern("input_column", "source"))
                .bind(5, asset)
                .run();
            std::lock_guard<std::mutex> lock(mutex_);
            inputThumb_[p.spanId] = small;
            continue;
        }
        // A span whose input frame was never taken has no root, and so no candidate.
        Statement candidate(db_, "INSERT OR IGNORE INTO maskscore_candidate SELECT ?, ?, 1, ?, ? "
                                 "WHERE EXISTS (SELECT 1 FROM maskscore_root WHERE span_id = ?)");
        candidate.bind(1, p.spanId).bind(2, edit).bind(3, asset).bind(4, intern("candidate_kind", "after_span"));
        candidate.bind(5, p.spanId).run();
        std::vector<uint8_t> before;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::map<int64_t, std::vector<uint8_t>>::iterator found = inputThumb_.find(p.spanId);
            if (found != inputThumb_.end())
            {
                before.swap(found->second);
                inputThumb_.erase(found);
            }
        }
        if (before.empty() || before.size() != small.size())
            continue;
        double sum = 0.0;
        for (size_t i = 0; i < small.size(); ++i)
            sum += std::abs(int(small[i]) - int(before[i]));
        Statement score(db_, "INSERT OR REPLACE INTO maskscore_score SELECT ?, ?, 1, ?, ? "
                             "WHERE EXISTS (SELECT 1 FROM maskscore_candidate WHERE span_id = ? AND rank = 1)");
        score.bind(1, p.spanId).bind(2, edit).bind(3, intern("metric", "gray_l1_8x8"));
        score.bind(4, sum / double(small.size()) / 255.0).bind(5, p.spanId).run();
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
