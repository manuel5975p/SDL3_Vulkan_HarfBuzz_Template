#include "format.hpp"

#include <cmath>
#include <cstdio>

namespace vkhb::ui {

std::string format_money(uint64_t amount) {
  std::string digits = std::to_string(amount);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) out += ' ';
    out += digits[i];
  }
  return out;
}

std::string format_pct(double v) {
  const int pct = static_cast<int>(std::lround(v * 100.0));
  return (pct > 0 ? "+" : "") + std::to_string(pct) + "%";
}

std::string format_mult(double v) {
  const double mult = v < 1.0 ? 1.0 + v : v;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "\xc3\x97%.2f", mult);  // "×" (U+00D7) UTF-8 = 0xC3 0x97
  return buf;
}

std::string format_one_decimal(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%s%.1f", v > 0 ? "+" : "", v);
  return buf;
}

}  // namespace vkhb::ui
