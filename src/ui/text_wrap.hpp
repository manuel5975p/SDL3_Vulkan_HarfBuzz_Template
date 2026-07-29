#pragma once

// Greedy word-wrap for laying out body text into a fixed-width column.

#include "ui_renderer.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vkhb::ui {

// Measures the advance width of one candidate line. A callback rather than a UiRenderer reference
// so wrapping stays testable (and reusable with any other measurer) without a Vulkan device.
using MeasureFn = std::function<float(std::string_view)>;

// Splits `text` on ASCII whitespace and greedily fills lines up to max_width.
// pre: max_width > 0. A single word wider than max_width still gets its own (overflowing) line —
// this never truncates or drops content.
std::vector<std::string> wrap_text(const MeasureFn& measure, std::string_view text, float max_width);

// Convenience overload measuring through a UiRenderer's font metrics.
std::vector<std::string> wrap_text(const UiRenderer& ui, std::string_view text, float max_width,
                                   const TextStyle& style = {});

}  // namespace vkhb::ui
