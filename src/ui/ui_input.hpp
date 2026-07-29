#pragma once

// UI-space geometry and per-frame pointer state, shared by UiRenderer and every widget built on it.

namespace vkhb::ui {

// Axis-aligned rectangle in UI space (physical pixels, origin top-left, +y down).
struct Rect {
  float x = 0, y = 0, w = 0, h = 0;

  constexpr bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  constexpr Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
  constexpr float cx() const { return x + w * 0.5f; }
  constexpr float cy() const { return y + h * 0.5f; }
  constexpr bool operator==(const Rect&) const = default;
};

// Pointer state for one frame, handed to UiRenderer::begin_frame(). `pressed`/`released` are edges
// (the button changed state this frame), `down` is the level — widgets need all three to tell a
// hover from a held press.
struct UiInput {
  float mouse_x = 0.0f, mouse_y = 0.0f;
  bool down = false;
  bool pressed = false;
  bool released = false;

  bool hovers(const Rect& r) const { return r.contains(mouse_x, mouse_y); }
  bool pressed_in(const Rect& r) const { return pressed && hovers(r); }
  bool released_in(const Rect& r) const { return released && hovers(r); }
};

}  // namespace vkhb::ui
