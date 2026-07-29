#include "ui/text_wrap.hpp"

#include "test_harness.hpp"

#include <string>

using vkhb::ui::wrap_text;

namespace {
// Fixed 10px-per-character measurer: makes expected line breaks exact without a font or a device.
float measure(std::string_view s) { return static_cast<float>(s.size()) * 10.0f; }
}  // namespace

int main() {
  CHECK(wrap_text(measure, "", 100.0f).empty());
  CHECK(wrap_text(measure, "   \t\n ", 100.0f).empty());

  {  // Fits on one line, whitespace collapsed to single spaces.
    const auto lines = wrap_text(measure, "a bb  ccc", 100.0f);
    CHECK(lines.size() == 1);
    CHECK(lines[0] == "a bb ccc");
  }
  {  // Breaks exactly when the candidate exceeds max_width (">" not ">=").
    const auto lines = wrap_text(measure, "aaa bbb ccc", 70.0f);  // "aaa bbb" = 70px, still fits
    CHECK(lines.size() == 2);
    CHECK(lines[0] == "aaa bbb");
    CHECK(lines[1] == "ccc");
  }
  {  // An over-long word overflows on its own line; nothing is dropped or truncated.
    const auto lines = wrap_text(measure, "hi supercalifragilistic bye", 50.0f);
    CHECK(lines.size() == 3);
    CHECK(lines[0] == "hi");
    CHECK(lines[1] == "supercalifragilistic");
    CHECK(lines[2] == "bye");
  }
  {  // Every word survives round-tripping, in order.
    const std::string text = "one two three four five six seven eight nine ten";
    std::string joined;
    for (const std::string& line : wrap_text(measure, text, 120.0f)) joined += (joined.empty() ? "" : " ") + line;
    CHECK(joined == text);
  }

  return TEST_MAIN_RESULT();
}
