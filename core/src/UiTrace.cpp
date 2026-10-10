// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/UiTrace.h"

#include "xrpilot/TraceStore.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>

namespace xrpilot
{

namespace
{

std::string quote(const char* s)
{
    std::string out = "\"";
    for (const char* p = s != nullptr ? s : ""; *p != '\0'; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\')
            out += '\\', out += char(c);
        else if (c < 0x20)
        {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\u%04x", c);
            out += hex;
        }
        else
            out += char(c);
    }
    return out + "\"";
}

} // namespace

void UiTrace::record(UiAction action, bool ok, const std::string& detail)
{
    if (store_ == nullptr)
        return;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    store_->mark("ui-" + std::to_string(++next_), uiSpanName(action), detail, ok, nowMs);
}

std::string uiTraceJsonl(sqlite3* db)
{
    std::string out;
    if (db == nullptr)
        return out;
    sqlite3_stmt* q = nullptr;
    const char* sql = "SELECT s.session_id, t.name, COALESCE(e.ok, 1), COALESCE(d.detail, '') "
                      "FROM span s JOIN tool t ON t.tool_id = s.tool_id "
                      "LEFT JOIN span_end e ON e.span_id = s.span_id "
                      "LEFT JOIN span_detail d ON d.span_id = s.span_id "
                      "WHERE t.name LIKE 'ui.%' ORDER BY s.session_id, s.span_id";
    if (sqlite3_prepare_v2(db, sql, -1, &q, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(q) == SQLITE_ROW)
    {
        out += "{\"session\":" + std::to_string(sqlite3_column_int64(q, 0)) +
               ",\"span\":" + quote(reinterpret_cast<const char*>(sqlite3_column_text(q, 1))) +
               ",\"ok\":" + (sqlite3_column_int(q, 2) != 0 ? "true" : "false");
        const char* detail = reinterpret_cast<const char*>(sqlite3_column_text(q, 3));
        if (detail != nullptr && detail[0] != '\0')
            out += ",\"detail\":" + quote(detail);
        out += "}\n";
    }
    sqlite3_finalize(q);
    return out;
}

} // namespace xrpilot
