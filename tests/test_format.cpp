#include "ui/format.hpp"

#include "test_harness.hpp"

using namespace vkhb::ui;

int main() {
  CHECK(format_money(0) == "0");
  CHECK(format_money(999) == "999");
  CHECK(format_money(1000) == "1 000");
  CHECK(format_money(1234567) == "1 234 567");
  CHECK(format_money(18446744073709551615ull) == "18 446 744 073 709 551 615");

  CHECK(format_pct(0.35) == "+35%");
  CHECK(format_pct(-0.1) == "-10%");
  CHECK(format_pct(0.0) == "0%");
  CHECK(format_pct(0.004) == "0%");  // rounds to zero, no sign

  CHECK(format_mult(0.25) == "\xc3\x97" "1.25");  // increments below 1 are treated as deltas
  CHECK(format_mult(2.0) == "\xc3\x97" "2.00");

  CHECK(format_one_decimal(0.6) == "+0.6");
  CHECK(format_one_decimal(-0.6) == "-0.6");
  CHECK(format_one_decimal(0.0) == "0.0");

  return vkhb::test::g_failures == 0 ? 0 : 1;
}
