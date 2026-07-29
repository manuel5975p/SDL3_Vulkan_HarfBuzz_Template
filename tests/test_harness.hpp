#pragma once

// Minimal assert-and-continue test harness: no vendored framework needed for this test surface.
// Each test file is its own main(); CHECK records a failure but keeps running so one file reports
// every mismatch in a run instead of stopping at the first.

#include <cmath>
#include <print>
#include <string_view>

namespace vkhb::test {
inline int g_failures = 0;

inline void check(bool cond, std::string_view expr, const char* file, int line) {
  if (!cond) {
    std::println(stderr, "CHECK FAILED: {} at {}:{}", expr, file, line);
    ++g_failures;
  }
}

inline void check_near(double actual, double expected, double eps, std::string_view expr, const char* file, int line) {
  if (std::abs(actual - expected) > eps) {
    std::println(stderr, "CHECK_NEAR FAILED: {} — actual {} vs expected {} (eps {}) at {}:{}", expr, actual, expected,
                 eps, file, line);
    ++g_failures;
  }
}

}  // namespace vkhb::test

#define CHECK(cond) ::vkhb::test::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected, eps) ::vkhb::test::check_near((actual), (expected), (eps), #actual " ~= " #expected, __FILE__, __LINE__)
#define TEST_MAIN_RESULT() (::vkhb::test::g_failures == 0 ? 0 : 1)
