#pragma once

// Locale-free number formatting for UI labels. Deliberately not locale-based: readouts want one
// fixed presentation regardless of the host locale.

#include <cstdint>
#include <string>

namespace vkhb::ui {

// 1234567 -> "1 234 567" (space-grouped thousands).
std::string format_money(uint64_t amount);

// Fraction to signed percent: 0.35 -> "+35%", -0.1 -> "-10%".
std::string format_pct(double v);

// Multiplier badge: 0.25 -> "×1.25", 2.0 -> "×2.00". pre: v < 1 is treated as an increment.
std::string format_mult(double v);

// One decimal with an explicit sign: 0.6 -> "+0.6".
std::string format_one_decimal(double v);

}  // namespace vkhb::ui
