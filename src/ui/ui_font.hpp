#pragma once

// GPU text via HarfBuzz's libharfbuzz-gpu: each unique glyph is shaped once, encoded by
// hb_gpu_draw_encode into a curve-band blob (resolution-independent, anti-aliased at any size), and
// appended to a shared atlas texel buffer that ui_text.vert/frag sample. Glyphs are cached forever
// — a UI's glyph set is finite, so a generously sized atlas never fills. Swap in an evicting atlas
// for unbounded text (CJK corpora, user-supplied fonts).

#include "../render/vk_buffer.hpp"

#include <hb-gpu.h>
#include <hb.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vkhb::render {
class Device;
}

namespace vkhb::ui {

// One glyph's cached ink extents/advance/atlas location, all in font design units (upem-scaled).
struct GlyphInfo {
  float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  float advance = 0;
  uint32_t atlas_offset = 0;
  bool is_empty = false;  // whitespace/invisible glyphs: advance applies, no quad is emitted
};

// The shared RGBA16I texel buffer every Font's encoded glyphs bump-allocate into (hb-gpu's atlas
// contract: `isamplerBuffer`, 8 bytes/texel). Bound read-only by ui_text.frag at binding 0.
class GlyphAtlas {
 public:
  // pre: capacity_texels * 8 fits comfortably in device memory (default 1<<20 texels = 8 MiB,
  //      matching hb-gpu's own demo tool's default).
  static std::expected<GlyphAtlas, std::string> create(const render::Device& dev,
                                                       uint32_t capacity_texels = 1u << 20);

  GlyphAtlas() = default;
  GlyphAtlas(GlyphAtlas&&) noexcept;
  GlyphAtlas& operator=(GlyphAtlas&&) noexcept;
  GlyphAtlas(const GlyphAtlas&) = delete;
  GlyphAtlas& operator=(const GlyphAtlas&) = delete;
  ~GlyphAtlas();

  // Appends len_bytes of RGBA16I texel data; returns the offset in texels.
  // pre: len_bytes % 8 == 0 and the remaining capacity covers it — exhaustion is a programming
  //      error (fixed, known glyph set), so this asserts rather than returning an error.
  uint32_t append(const void* data, uint32_t len_bytes);

  VkBufferView view() const { return view_; }

 private:
  void destroy() noexcept;

  const VolkDeviceTable* vk_ = nullptr;
  VkDevice device_ = VK_NULL_HANDLE;
  render::Buffer buffer_;
  VkBufferView view_ = VK_NULL_HANDLE;
  uint32_t capacity_texels_ = 0;
  uint32_t cursor_texels_ = 0;
};

// One positioned glyph from shape(), in font design units (not yet scaled to pixels).
struct ShapedGlyph {
  uint32_t glyph_index = 0;
  float x_offset = 0, y_offset = 0, x_advance = 0;
};

// hb_face_t/hb_font_t for one font weight, plus its glyph cache and hb_gpu_draw_t scratch encoder.
// Not thread-safe (matches the rest of this single-threaded renderer).
class Font {
 public:
  // pre: asset_name is a logical asset name ("fonts/Inter-Regular.ttf"); atlas outlives the Font.
  static std::expected<Font, std::string> create(std::string_view asset_name, GlyphAtlas& atlas);

  Font() = default;
  Font(Font&&) noexcept;
  Font& operator=(Font&&) noexcept;
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;
  ~Font();

  // Shapes utf8 as a single LTR run (the game's UI text is all English/numerals — no bidi or
  // script itemization needed). Returns positions in font design units.
  std::vector<ShapedGlyph> shape(std::string_view utf8) const;

  // Looks up (encoding into the atlas on first use) a glyph's cached extents/advance/atlas offset.
  const GlyphInfo& glyph_info(uint32_t glyph_index) const;

  unsigned int upem() const { return upem_; }
  float ascent_em() const { return ascent_em_; }    // font units above the baseline
  float descent_em() const { return descent_em_; }  // font units below the baseline (positive)

 private:
  void destroy() noexcept;

  hb_face_t* face_ = nullptr;
  hb_font_t* font_ = nullptr;
  hb_gpu_draw_t* draw_ = nullptr;
  GlyphAtlas* atlas_ = nullptr;
  unsigned int upem_ = 0;
  float ascent_em_ = 0;
  float descent_em_ = 0;
  mutable std::unordered_map<uint32_t, GlyphInfo> cache_;
};

}  // namespace vkhb::ui
