// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// xrpilot-ui-trace: writes the UI spans a pilot recorded as JSON lines, for `lake exe ui_check`.
//   xrpilot-ui-trace <traces.sqlite> [out.jsonl]

#include "xrpilot/UiTrace.h"

#include <sqlite3.h>

#include <cstdio>
#include <fstream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: xrpilot-ui-trace <traces.sqlite> [out.jsonl]\n");
        return 2;
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        std::fprintf(stderr, "xrpilot-ui-trace: cannot open %s\n", argv[1]);
        sqlite3_close(db);
        return 1;
    }
    const std::string lines = xrpilot::uiTraceJsonl(db);
    sqlite3_close(db);
    if (argc > 2)
        std::ofstream(argv[2], std::ios::binary | std::ios::trunc) << lines;
    else
        std::fwrite(lines.data(), 1, lines.size(), stdout);
    return 0;
}
