// SPDX-License-Identifier: Apache-2.0 OR MIT
#include <cstdio>
#include <cstring>

#include "check.h"

// With no argument every case but the harness controls runs; with one, only that case.
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--list") == 0) {
        for (const check::Case& c : check::registry()) std::printf("%s\n", c.name.c_str());
        return 0;
    }
    int ran = 0;
    for (const check::Case& c : check::registry()) {
        if (argc > 1 ? c.name != argv[1] : c.name.rfind("control.", 0) == 0) continue;
        int before = check::counts().failed;
        int checksBefore = check::counts().passed + before;
        c.body();
        ++ran;
        int checks = check::counts().passed + check::counts().failed - checksBefore;
        if (checks == 0) {
            std::fprintf(stderr, "FAIL %s: ran zero checks\n", c.name.c_str());
            ++check::counts().failed;
        }
        std::printf("%s %s (%d checks)\n", check::counts().failed == before ? "PASS" : "FAIL", c.name.c_str(), checks);
    }
    if (ran == 0) {
        std::fprintf(stderr, "FAIL no case named '%s'\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    std::printf("%d passed, %d failed, %d cases\n", check::counts().passed, check::counts().failed, ran);
    return check::counts().failed == 0 ? 0 : 1;
}
