#pragma once

// Shared modal-panel chrome (dim backdrop, rounded frame, header with title/subtitle/close button),
// drawn from UiRenderer primitives. One call per open panel per frame.

#include "ui_renderer.hpp"

#include <string_view>

namespace vkhb::ui {

struct PanelChrome {
  Rect body;                     // content area below the header
  Rect header;                   // full header strip
  float header_extra_right_x = 0;  // right edge for the caller's own header widgets
  bool wants_close = false;      // close button or backdrop clicked this frame
};

// pre: called once per open panel, every frame, after ui.begin_frame().
PanelChrome draw_panel_chrome(UiRenderer& ui, VkExtent2D extent, std::string_view title, std::string_view subtitle,
                              float frame_width_cap = 1180.0f);

}  // namespace vkhb::ui
