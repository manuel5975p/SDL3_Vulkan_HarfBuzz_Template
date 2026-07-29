#include "panel_chrome.hpp"

#include "theme.hpp"

#include <algorithm>
#include <cctype>

namespace vkhb::ui {

namespace {

std::string to_upper(std::string_view s) {
  std::string out(s);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return out;
}

}  // namespace

PanelChrome draw_panel_chrome(UiRenderer& ui, VkExtent2D extent, std::string_view title, std::string_view subtitle,
                              float frame_width_cap) {
  const float ew = static_cast<float>(extent.width), eh = static_cast<float>(extent.height);

  // Backdrop: dims whatever is behind the panel; clicking outside the frame closes it.
  ui.rect({0, 0, ew, eh}, theme::backdrop_dim());
  const bool backdrop_pressed = ui.input().pressed;

  const float frame_w = std::min(frame_width_cap, ew * 0.94f);
  const float frame_h = eh * 0.9f;
  const Rect frame{(ew - frame_w) * 0.5f, (eh - frame_h) * 0.5f, frame_w, frame_h};
  ui.rect(frame, theme::frame_fill(), 16.0f);

  constexpr float kHeaderH = 56.0f;
  constexpr float kPad = 20.0f;
  const Rect header{frame.x, frame.y, frame.w, kHeaderH};
  ui.line(frame.x + 4.0f, frame.y + kHeaderH, frame.x + frame.w - 4.0f, frame.y + kHeaderH, 1.0f, theme::edge());

  const TextStyle title_style{.size = 17.0f, .color = theme::accent(), .weight = FontWeight::Bold};
  const std::string heading = to_upper(title);
  const float title_baseline = header.cy() + ui.ascent(title_style) * 0.5f;
  ui.text(frame.x + kPad, title_baseline, heading, title_style);
  ui.text(frame.x + kPad + ui.measure_text(heading, title_style) + 14.0f, title_baseline, subtitle,
          {.size = 13.0f, .color = theme::text_faint()});

  // Close button: a pill-radius button is a circle, so this reuses the widget rather than
  // hand-rolling its own hover test.
  constexpr float kCloseR = 16.0f;
  const Rect close{frame.x + frame.w - kPad - 2 * kCloseR, header.cy() - kCloseR, 2 * kCloseR, 2 * kCloseR};
  const bool close_clicked = ui.button(close, "\xc3\x97",  // "×"
                                       {.radius = kCloseR,
                                        .fill = theme::glass_hi(),
                                        .hover_fill = theme::danger(),
                                        .active_fill = theme::danger(),
                                        .label = {.size = 15.0f, .weight = FontWeight::Bold}});

  const float body_top = frame.y + kHeaderH + 18.0f;
  return {
      .body = {frame.x + kPad, body_top, frame.w - 2.0f * kPad, frame.y + frame.h - body_top - 22.0f},
      .header = header,
      .header_extra_right_x = close.x - 14.0f,
      .wants_close = close_clicked || (backdrop_pressed && !ui.input().hovers(frame)),
  };
}

}  // namespace vkhb::ui
