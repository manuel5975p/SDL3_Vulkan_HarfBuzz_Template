#include "ui_font.hpp"

#include "../render/vk_context.hpp"
#include "../render/vk_error.hpp"
#include "assets/assets.hpp"

#include <cassert>
#include <utility>

namespace vkhb::ui {

using vkhb::render::vk_error;

// --- GlyphAtlas ---

std::expected<GlyphAtlas, std::string> GlyphAtlas::create(const render::Device& dev, uint32_t capacity_texels) {
  constexpr VkDeviceSize kTexelSize = 8;  // sizeof RGBA16I texel, per hb-gpu's atlas contract
  auto buffer = render::Buffer::create(dev, VkDeviceSize{capacity_texels} * kTexelSize,
                                       VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!buffer) return std::unexpected(buffer.error());

  const VkDevice device = dev.handle();
  GlyphAtlas atlas;
  atlas.vk_ = &dev.vk();
  atlas.device_ = device;
  atlas.buffer_ = std::move(*buffer);
  atlas.capacity_texels_ = capacity_texels;

  const VkBufferViewCreateInfo view_info{
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = atlas.buffer_.handle(),
      .format = VK_FORMAT_R16G16B16A16_SINT,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
  };
  if (const VkResult r = dev.vk().vkCreateBufferView(device, &view_info, nullptr, &atlas.view_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateBufferView (glyph atlas)", r));

  return atlas;
}

uint32_t GlyphAtlas::append(const void* data, uint32_t len_bytes) {
  assert(len_bytes % 8 == 0 && "hb-gpu blobs are always a whole number of RGBA16I texels");
  const uint32_t len_texels = len_bytes / 8;
  assert(cursor_texels_ + len_texels <= capacity_texels_ && "glyph atlas exhausted — raise capacity_texels");

  const uint32_t offset = cursor_texels_;
  buffer_.upload_at(VkDeviceSize{offset} * 8, data, len_bytes);
  cursor_texels_ += len_texels;
  return offset;
}

void GlyphAtlas::destroy() noexcept {
  if (view_) vk_->vkDestroyBufferView(device_, view_, nullptr);
  view_ = VK_NULL_HANDLE;
}

GlyphAtlas::GlyphAtlas(GlyphAtlas&& other) noexcept
    : vk_(std::exchange(other.vk_, nullptr)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      buffer_(std::move(other.buffer_)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)),
      capacity_texels_(std::exchange(other.capacity_texels_, 0)),
      cursor_texels_(std::exchange(other.cursor_texels_, 0)) {}

GlyphAtlas& GlyphAtlas::operator=(GlyphAtlas&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  vk_ = std::exchange(other.vk_, nullptr);
  device_ = std::exchange(other.device_, VK_NULL_HANDLE);
  buffer_ = std::move(other.buffer_);
  view_ = std::exchange(other.view_, VK_NULL_HANDLE);
  capacity_texels_ = std::exchange(other.capacity_texels_, 0);
  cursor_texels_ = std::exchange(other.cursor_texels_, 0);
  return *this;
}

GlyphAtlas::~GlyphAtlas() { destroy(); }

// --- Font ---

std::expected<Font, std::string> Font::create(std::string_view asset_name, GlyphAtlas& atlas) {
  const std::string name(asset_name);
  auto bytes = assets::load(asset_name);
  if (!bytes) return std::unexpected(bytes.error());

  // HB_MEMORY_MODE_READONLY, no destroy callback: the asset layer promises the bytes outlive the
  // process, so HarfBuzz can reference them in place instead of taking a copy of every face.
  hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(bytes->data()),
                                   static_cast<unsigned int>(bytes->size()), HB_MEMORY_MODE_READONLY, nullptr,
                                   nullptr);
  hb_face_t* face = hb_face_create(blob, 0);
  hb_blob_destroy(blob);
  if (face == hb_face_get_empty()) {
    hb_face_destroy(face);
    return std::unexpected("failed to parse font face: " + name);
  }

  Font font;
  font.face_ = face;
  font.font_ = hb_font_create(face);
  font.atlas_ = &atlas;
  font.draw_ = hb_gpu_draw_create_or_fail();
  if (!font.draw_) {
    font.destroy();
    return std::unexpected("hb_gpu_draw_create_or_fail failed");
  }

  int x_scale = 0, y_scale = 0;
  hb_font_get_scale(font.font_, &x_scale, &y_scale);
  font.upem_ = static_cast<unsigned int>(y_scale);  // default scale == face upem (no set_scale call)

  hb_font_extents_t extents{};
  hb_font_get_h_extents(font.font_, &extents);
  font.ascent_em_ = static_cast<float>(extents.ascender);
  font.descent_em_ = static_cast<float>(-extents.descender);  // HarfBuzz: descender is negative

  return font;
}

void Font::destroy() noexcept {
  if (draw_) hb_gpu_draw_destroy(draw_);
  if (font_) hb_font_destroy(font_);
  if (face_) hb_face_destroy(face_);
  draw_ = nullptr;
  font_ = nullptr;
  face_ = nullptr;
}

Font::Font(Font&& other) noexcept
    : face_(std::exchange(other.face_, nullptr)),
      font_(std::exchange(other.font_, nullptr)),
      draw_(std::exchange(other.draw_, nullptr)),
      atlas_(std::exchange(other.atlas_, nullptr)),
      upem_(std::exchange(other.upem_, 0)),
      ascent_em_(std::exchange(other.ascent_em_, 0.0f)),
      descent_em_(std::exchange(other.descent_em_, 0.0f)),
      cache_(std::move(other.cache_)) {}

Font& Font::operator=(Font&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  face_ = std::exchange(other.face_, nullptr);
  font_ = std::exchange(other.font_, nullptr);
  draw_ = std::exchange(other.draw_, nullptr);
  atlas_ = std::exchange(other.atlas_, nullptr);
  upem_ = std::exchange(other.upem_, 0);
  ascent_em_ = std::exchange(other.ascent_em_, 0.0f);
  descent_em_ = std::exchange(other.descent_em_, 0.0f);
  cache_ = std::move(other.cache_);
  return *this;
}

Font::~Font() { destroy(); }

std::vector<ShapedGlyph> Font::shape(std::string_view utf8) const {
  hb_buffer_t* buf = hb_buffer_create();
  hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()), 0, -1);
  hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
  hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
  hb_buffer_set_language(buf, hb_language_from_string("en", -1));
  hb_shape(font_, buf, nullptr, 0);

  unsigned int count = 0;
  hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
  hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buf, nullptr);

  std::vector<ShapedGlyph> out;
  out.reserve(count);
  for (unsigned int i = 0; i < count; ++i) {
    out.push_back({.glyph_index = infos[i].codepoint,
                   .x_offset = static_cast<float>(positions[i].x_offset),
                   .y_offset = static_cast<float>(positions[i].y_offset),
                   .x_advance = static_cast<float>(positions[i].x_advance)});
  }
  hb_buffer_destroy(buf);
  return out;
}

const GlyphInfo& Font::glyph_info(uint32_t glyph_index) const {
  if (auto it = cache_.find(glyph_index); it != cache_.end()) return it->second;

  hb_gpu_draw_clear(draw_);
  hb_gpu_draw_glyph(draw_, font_, glyph_index);
  hb_glyph_extents_t ext{};
  hb_blob_t* blob = hb_gpu_draw_encode(draw_, &ext);
  const unsigned int len = blob ? hb_blob_get_length(blob) : 0;

  GlyphInfo info{
      .min_x = static_cast<float>(ext.x_bearing),
      .min_y = static_cast<float>(ext.y_bearing + ext.height),
      .max_x = static_cast<float>(ext.x_bearing + ext.width),
      .max_y = static_cast<float>(ext.y_bearing),
      .advance = static_cast<float>(hb_font_get_glyph_h_advance(font_, glyph_index)),
      .atlas_offset = 0,
      .is_empty = (len == 0),
  };
  if (!info.is_empty) {
    unsigned int data_len = 0;
    const char* data = hb_blob_get_data(blob, &data_len);
    info.atlas_offset = atlas_->append(data, data_len);
  }
  if (blob) hb_gpu_draw_recycle_blob(draw_, blob);

  return cache_.emplace(glyph_index, info).first->second;
}

}  // namespace vkhb::ui
