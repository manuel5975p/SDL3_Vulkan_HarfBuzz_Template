#pragma once

// Immediate-mode 2D UI: begin_frame(), then any mix of rect()/line()/circle()/text()/button(),
// then render(). Coordinates are physical pixels, origin top-left, +y down. Draw data is rebuilt
// from scratch every frame — a game UI redraws anyway, so there is no retained scene graph.

#include "../render/vk_buffer.hpp"
#include "theme.hpp"
#include "ui_font.hpp"
#include "ui_input.hpp"
#include "ui_pipeline.hpp"
#include "ui_vertex.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

#include <array>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vkhb::render {
class Device;
}

namespace vkhb::ui {

inline constexpr uint32_t kUiFramesInFlight = 2;  // must match render::kFramesInFlight

enum class FontWeight { Regular, SemiBold, Bold };

// Everything about drawing a string except where it goes. Designed for designated initializers:
// `ui.text(x, y, "Hello", {.size = 20, .color = theme::accent()})`.
struct TextStyle {
  float size = 14.0f;
  glm::vec4 color = theme::text();
  FontWeight weight = FontWeight::Regular;
};

// Fills for a button's three states, plus its label style.
struct ButtonStyle {
  float radius = 8.0f;
  glm::vec4 fill = theme::ink600();
  glm::vec4 hover_fill = theme::ink500();
  glm::vec4 active_fill = theme::accent_dim();
  TextStyle label{.size = 14.0f, .color = theme::text(), .weight = FontWeight::SemiBold};
};

class UiRenderer {
 public:
  // pre: dev outlives the renderer; color_format matches the attachment render() draws into; the
  //      "shaders/ui_*" and "fonts/Inter-*" assets resolve (see assets/assets.hpp).
  static std::expected<UiRenderer, std::string> create(const render::Device& dev, VkFormat color_format);

  UiRenderer() = default;
  // Not defaulted: pipelines_/descriptor_pool_ are raw Vulkan handles with no RAII of their own.
  UiRenderer(UiRenderer&& other) noexcept;
  UiRenderer& operator=(UiRenderer&& other) noexcept;
  UiRenderer(const UiRenderer&) = delete;
  UiRenderer& operator=(const UiRenderer&) = delete;
  ~UiRenderer();

  // Starts a frame and latches this frame's pointer state, so widgets need not be handed it one by
  // one. pre: called once per frame, before any drawing call.
  void begin_frame(VkExtent2D extent, const UiInput& input = {});

  // This frame's pointer state, for callers hit-testing their own non-widget regions.
  const UiInput& input() const { return input_; }

  // Signed-distance rounded rect (radius 0 = sharp; radius >= min(w,h)/2 = pill/circle).
  void rect(const Rect& r, glm::vec4 color, float radius = 0.0f);

  // A thin rounded-cap line segment, drawn as a rotated rect (tree connectors, map trails).
  void line(float x0, float y0, float x1, float y1, float thickness, glm::vec4 color);

  void circle(float cx, float cy, float radius, glm::vec4 color) {
    rect({cx - radius, cy - radius, 2 * radius, 2 * radius}, color, radius);
  }

  // Draws left-baseline-anchored text: (x,y) is where the baseline sits, so descenders fall below
  // y. Returns the horizontal advance.
  float text(float x, float y, std::string_view utf8, const TextStyle& style = {});

  // Draws `utf8` centred in `r` on its ink, not its metrics — so a symbol whose glyph sits off the
  // usual cap band (\xc3\x97, arrows, dingbats) lands optically centred instead of riding low.
  void text_centered(const Rect& r, std::string_view utf8, const TextStyle& style = {});

  // The advance text() would produce, without drawing (for centring/right-alignment).
  float measure_text(std::string_view utf8, const TextStyle& style = {}) const;

  // Tight bounding box of the marks `utf8` actually paints, in pixels relative to the text origin:
  // +x right, +y *up* from the baseline. has_ink is false for strings that draw nothing.
  struct InkBox {
    float min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    bool has_ink = false;
  };
  InkBox measure_ink(std::string_view utf8, const TextStyle& style = {}) const;

  float ascent(const TextStyle& style) const;
  float descent(const TextStyle& style) const;

  // Draws a labelled button and reports whether it was clicked this frame. A click is a press and
  // a release both inside `r`, so dragging off the button cancels it, as users expect.
  // pre: begin_frame() was given this frame's real input; `r` is stable frame to frame (the rect
  //      doubles as the widget's identity — two buttons must not share one).
  bool button(const Rect& r, std::string_view label, const ButtonStyle& style = {});

  // Records this frame's draw data into `cmd` as its own dynamic-rendering pass (LOAD_OP_LOAD onto
  // color_view, no depth) — call after the scene pass.
  // pre: frame_index < kUiFramesInFlight, matching the caller's frame-in-flight index.
  void render(VkCommandBuffer cmd, uint32_t frame_index, VkImageView color_view, VkExtent2D extent);

 private:
  void destroy() noexcept;
  Font& font_for(FontWeight weight);
  const Font& font_for(FontWeight weight) const;
  void emit_glyph_quad(float px, float py, float scale, const GlyphInfo& gi);

  struct TextRun {
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
    glm::vec4 color{1.0f};
  };

  const VolkDeviceTable* vk_ = nullptr;
  VkDevice device_ = VK_NULL_HANDLE;

  // Heap-allocated: each Font stores a raw GlyphAtlas* it appends encoded glyphs into, so the
  // atlas needs a stable address across UiRenderer moves.
  std::unique_ptr<GlyphAtlas> atlas_;
  std::array<Font, 3> fonts_;  // indexed by FontWeight

  UiPipelineSet pipelines_;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kUiFramesInFlight> descriptor_sets_{};
  std::array<render::Buffer, kUiFramesInFlight> frame_ubos_;
  std::array<render::Buffer, kUiFramesInFlight> rect_vertex_buffers_;
  std::array<render::Buffer, kUiFramesInFlight> glyph_vertex_buffers_;

  // Per-frame scratch, rebuilt in begin_frame() and consumed by render().
  std::vector<RectVertex> rect_vertices_;
  std::vector<GlyphVertex> glyph_vertices_;
  std::vector<TextRun> text_runs_;
  glm::mat4 mvp_{1.0f};
  VkExtent2D extent_{0, 0};

  // The button currently held down, identified by its rect. Survives across frames — that is the
  // whole point, since press and release land in different frames.
  UiInput input_;
  Rect active_button_{};
  bool has_active_button_ = false;
};

}  // namespace vkhb::ui
