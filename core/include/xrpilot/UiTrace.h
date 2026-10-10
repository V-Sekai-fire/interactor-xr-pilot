// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Records each UI action as a span of the trace store, named by UiSpans.h (generated from the Lean UI
// model), so a recorded run can be checked against that model: `xrpilot-ui-trace traces.sqlite` writes
// the spans as JSON lines, and `lake exe ui_check` replays them.

#pragma once

#include "xrpilot/UiSpans.h"

#include <atomic>
#include <cstdint>
#include <string>

struct sqlite3;

namespace xrpilot
{

class TraceStore;

class UiTrace
{
public:
    explicit UiTrace(TraceStore* store = nullptr) : store_(store) {}
    void attach(TraceStore* store) { store_ = store; }
    void record(UiAction action, bool ok = true, const std::string& detail = std::string());

private:
    TraceStore* store_ = nullptr;
    std::atomic<uint64_t> next_{0};
};

// The ui.* spans of every session, one JSON object per line in the order they started:
// {"session":1,"span":"ui.launch.pilot","ok":true,"detail":"..."}.
std::string uiTraceJsonl(sqlite3* db);

} // namespace xrpilot
