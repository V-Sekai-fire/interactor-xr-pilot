// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Counts {
    int passed = 0;
    int failed = 0;
};

inline Counts& counts() {
    static Counts c;
    return c;
}

inline void record(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
        ++counts().passed;
    } else {
        ++counts().failed;
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    }
}

struct Case {
    std::string name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back(Case{name, std::move(body)}); }
};

}

#define CHECK(expr) ::check::record(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::check::record((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST_CASE(name)                                                                              \
    static void CHECK_CAT(test_fn_, __LINE__)();                                                     \
    static ::check::Registrar CHECK_CAT(test_reg_, __LINE__)(name, &CHECK_CAT(test_fn_, __LINE__)); \
    static void CHECK_CAT(test_fn_, __LINE__)()
