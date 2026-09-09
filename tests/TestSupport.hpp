// tests/TestSupport.hpp - a two-macro test harness.
//
// The module has no test framework dependency for the same reason it has no
// JSON dependency: the tests are few, they are numeric, and a failing CHECK with
// a file and line is all the reporting they need.
#pragma once

#include <cstdio>
#include <string>

namespace test {

inline int failures = 0;

inline void report(const char* file, int line, const char* expr, const std::string& detail) {
    ++failures;
    std::fprintf(stderr, "FAIL %s:%d  %s%s%s\n", file, line, expr,
                 detail.empty() ? "" : "  -- ", detail.c_str());
}

inline int summary(const char* name) {
    if (failures == 0) {
        std::printf("PASS %s\n", name);
        return 0;
    }
    std::fprintf(stderr, "%d failure(s) in %s\n", failures, name);
    return 1;
}

}  // namespace test

#define CHECK(expr)                                            \
    do {                                                       \
        if (!(expr)) test::report(__FILE__, __LINE__, #expr, ""); \
    } while (false)

#define CHECK_NEAR(actual, expected, tol)                                         \
    do {                                                                          \
        const double a_ = (actual);                                               \
        const double e_ = (expected);                                             \
        if (!(std::fabs(a_ - e_) <= (tol))) {                                     \
            test::report(__FILE__, __LINE__, #actual " ~= " #expected,             \
                         "got " + std::to_string(a_) + ", expected " +            \
                             std::to_string(e_) + " +/- " + std::to_string(tol)); \
        }                                                                         \
    } while (false)
