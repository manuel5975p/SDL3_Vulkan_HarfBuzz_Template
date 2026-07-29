#include "text_wrap.hpp"

#include <sstream>

namespace vkhb::ui {

std::vector<std::string> wrap_text(const MeasureFn& measure, std::string_view text, float max_width) {
  std::vector<std::string> lines;
  std::istringstream words{std::string(text)};
  std::string word, current;

  const auto flush = [&] {
    if (!current.empty()) lines.push_back(current);
    current.clear();
  };

  while (words >> word) {
    const std::string candidate = current.empty() ? word : current + " " + word;
    if (!current.empty() && measure(candidate) > max_width) {
      flush();
      current = word;
    } else {
      current = candidate;
    }
  }
  flush();
  return lines;
}

std::vector<std::string> wrap_text(const UiRenderer& ui, std::string_view text, float max_width,
                                   const TextStyle& style) {
  return wrap_text([&](std::string_view s) { return ui.measure_text(s, style); }, text, max_width);
}

}  // namespace vkhb::ui
