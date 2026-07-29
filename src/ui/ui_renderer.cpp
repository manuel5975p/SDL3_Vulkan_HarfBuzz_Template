#include "ui_renderer.hpp"

#include "../render/vk_context.hpp"
#include "../render/vk_error.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vkhb::ui {

using vkhb::render::vk_error;

namespace {

struct UiFrameUbo {
  glm::mat4 mvp{1.0f};
  glm::vec2 viewport{0.0f};
};

}  // namespace

std::expected<UiRenderer, std::string> UiRenderer::create(const render::Device& dev, VkFormat color_format) {
  const VolkDeviceTable& vk = dev.vk();
  UiRenderer r;
  r.vk_ = &vk;
  r.device_ = dev.handle();

  auto atlas = GlyphAtlas::create(dev);
  if (!atlas) return std::unexpected(atlas.error());
  r.atlas_ = std::make_unique<GlyphAtlas>(std::move(*atlas));

  const std::array<std::string_view, 3> weight_files{"fonts/Inter-Regular.ttf", "fonts/Inter-SemiBold.ttf",
                                                     "fonts/Inter-Bold.ttf"};
  for (size_t i = 0; i < weight_files.size(); ++i) {
    auto font = Font::create(weight_files[i], *r.atlas_);
    if (!font) return std::unexpected(font.error());
    r.fonts_[i] = std::move(*font);
  }

  auto pipelines = create_ui_pipelines(dev, color_format);
  if (!pipelines) return std::unexpected(pipelines.error());
  r.pipelines_ = std::move(*pipelines);

  constexpr VkDeviceSize kRectBufferBytes = 4096 * 6 * sizeof(RectVertex);   // ~1 MiB, 4096 rects/frame
  constexpr VkDeviceSize kGlyphBufferBytes = 8192 * 6 * sizeof(GlyphVertex);  // ~1.5 MiB, 8192 glyphs/frame
  for (uint32_t i = 0; i < kUiFramesInFlight; ++i) {
    auto ubo = render::Buffer::create(dev, sizeof(UiFrameUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!ubo) return std::unexpected(ubo.error());
    r.frame_ubos_[i] = std::move(*ubo);

    auto rect_buf = render::Buffer::create(dev, kRectBufferBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!rect_buf) return std::unexpected(rect_buf.error());
    r.rect_vertex_buffers_[i] = std::move(*rect_buf);

    auto glyph_buf = render::Buffer::create(dev, kGlyphBufferBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!glyph_buf) return std::unexpected(glyph_buf.error());
    r.glyph_vertex_buffers_[i] = std::move(*glyph_buf);
  }

  const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .descriptorCount = kUiFramesInFlight},
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = kUiFramesInFlight},
  }};
  const VkDescriptorPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = kUiFramesInFlight,
      .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
      .pPoolSizes = pool_sizes.data(),
  };
  if (const VkResult res = vk.vkCreateDescriptorPool(r.device_, &pool_info, nullptr, &r.descriptor_pool_);
      res != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateDescriptorPool (ui)", res));

  std::array<VkDescriptorSetLayout, kUiFramesInFlight> layouts;
  layouts.fill(r.pipelines_.descriptor_set_layout);
  const VkDescriptorSetAllocateInfo set_alloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = r.descriptor_pool_,
      .descriptorSetCount = kUiFramesInFlight,
      .pSetLayouts = layouts.data(),
  };
  if (const VkResult res = vk.vkAllocateDescriptorSets(r.device_, &set_alloc, r.descriptor_sets_.data());
      res != VK_SUCCESS)
    return std::unexpected(vk_error("vkAllocateDescriptorSets (ui)", res));

  const VkBufferView atlas_view = r.atlas_->view();
  for (uint32_t i = 0; i < kUiFramesInFlight; ++i) {
    const VkDescriptorBufferInfo ubo_info{.buffer = r.frame_ubos_[i].handle(), .range = sizeof(UiFrameUbo)};
    const std::array<VkWriteDescriptorSet, 2> writes{{
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = r.descriptor_sets_[i],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
         .pTexelBufferView = &atlas_view},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = r.descriptor_sets_[i],
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .pBufferInfo = &ubo_info},
    }};
    vk.vkUpdateDescriptorSets(r.device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  }

  return r;
}

Font& UiRenderer::font_for(FontWeight weight) { return fonts_[static_cast<size_t>(weight)]; }
const Font& UiRenderer::font_for(FontWeight weight) const { return fonts_[static_cast<size_t>(weight)]; }

void UiRenderer::begin_frame(VkExtent2D extent, const UiInput& input) {
  extent_ = extent;
  input_ = input;
  // Forget the held button once the pointer is genuinely up. Not on the release frame itself:
  // button() still has to see the press/release pair to report the click.
  if (!input_.down && !input_.released) has_active_button_ = false;
  // Pixel-space orthographic projection: origin top-left, +y down, matching screen conventions
  // (no GLM_FORCE_DEPTH_ZERO_TO_ONE concern here — z is unused, depth test is off for this pass).
  mvp_ = glm::mat4(1.0f);
  mvp_[0][0] = 2.0f / static_cast<float>(extent.width);
  mvp_[1][1] = 2.0f / static_cast<float>(extent.height);
  mvp_[3][0] = -1.0f;
  mvp_[3][1] = -1.0f;

  rect_vertices_.clear();
  glyph_vertices_.clear();
  text_runs_.clear();
}

void UiRenderer::rect(const Rect& r_in, glm::vec4 color, float radius) {
  const float half_w = r_in.w * 0.5f, half_h = r_in.h * 0.5f;
  const float cx = r_in.x + half_w, cy = r_in.y + half_h;
  const float r = std::min(radius, std::min(half_w, half_h));

  RectVertex corners[4];
  for (int ci = 0; ci < 4; ++ci) {
    const float sx = (ci & 1) ? 1.0f : -1.0f;
    const float sy = (ci & 2) ? 1.0f : -1.0f;
    corners[ci] = {.x = cx + sx * half_w,
                   .y = cy + sy * half_h,
                   .local_x = sx * half_w,
                   .local_y = sy * half_h,
                   .half_w = half_w,
                   .half_h = half_h,
                   .radius = r,
                   .r = color.r,
                   .g = color.g,
                   .b = color.b,
                   .a = color.a};
  }
  // Two triangles: (0,1,2) and (1,2,3) — matches the (-,-)(+,-)(-,+)(+,+) corner order above.
  for (int idx : {0, 1, 2, 1, 2, 3}) rect_vertices_.push_back(corners[idx]);
}

void UiRenderer::line(float x0, float y0, float x1, float y1, float thickness, glm::vec4 color) {
  const float dx = x1 - x0, dy = y1 - y0;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-4f) return;
  const float angle = std::atan2(dy, dx);
  const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
  const float half_w = len * 0.5f, half_h = thickness * 0.5f;
  const float cos_a = std::cos(angle), sin_a = std::sin(angle);

  RectVertex corners[4];
  for (int ci = 0; ci < 4; ++ci) {
    const float sx = (ci & 1) ? 1.0f : -1.0f;
    const float sy = (ci & 2) ? 1.0f : -1.0f;
    const float lx = sx * half_w, ly = sy * half_h;
    corners[ci] = {.x = cx + lx * cos_a - ly * sin_a,
                   .y = cy + lx * sin_a + ly * cos_a,
                   .local_x = lx,
                   .local_y = ly,
                   .half_w = half_w,
                   .half_h = half_h,
                   .radius = half_h,  // rounded caps
                   .r = color.r,
                   .g = color.g,
                   .b = color.b,
                   .a = color.a};
  }
  for (int idx : {0, 1, 2, 1, 2, 3}) rect_vertices_.push_back(corners[idx]);
}

void UiRenderer::emit_glyph_quad(float px, float py, float scale, const GlyphInfo& gi) {
  GlyphVertex corners[4];
  for (int ci = 0; ci < 4; ++ci) {
    const int cx = (ci >> 1) & 1;
    const int cy = ci & 1;
    const float ex = (1 - cx) * gi.min_x + cx * gi.max_x;
    const float ey = (1 - cy) * gi.min_y + cy * gi.max_y;
    corners[ci] = {.x = px + scale * ex,
                   .y = py - scale * ey,
                   .tx = ex,
                   .ty = ey,
                   .nx = cx ? 1.0f : -1.0f,
                   .ny = cy ? -1.0f : 1.0f,
                   .em_per_pos = 1.0f / scale,
                   .atlas_offset = gi.atlas_offset};
  }
  for (int idx : {0, 1, 2, 1, 2, 3}) glyph_vertices_.push_back(corners[idx]);
}

float UiRenderer::text(float x, float y, std::string_view utf8, const TextStyle& style) {
  const Font& font = font_for(style.weight);
  const auto glyphs = font.shape(utf8);
  const float scale = style.size / static_cast<float>(font.upem());

  const uint32_t start = static_cast<uint32_t>(glyph_vertices_.size());
  float cursor_x = x, cursor_y = y;
  for (const auto& g : glyphs) {
    const GlyphInfo& gi = font.glyph_info(g.glyph_index);
    if (!gi.is_empty) emit_glyph_quad(cursor_x + scale * g.x_offset, cursor_y - scale * g.y_offset, scale, gi);
    cursor_x += scale * g.x_advance;
  }
  const uint32_t count = static_cast<uint32_t>(glyph_vertices_.size()) - start;
  if (count > 0) text_runs_.push_back({.vertex_offset = start, .vertex_count = count, .color = style.color});
  return cursor_x - x;
}

float UiRenderer::measure_text(std::string_view utf8, const TextStyle& style) const {
  const Font& font = font_for(style.weight);
  const float scale = style.size / static_cast<float>(font.upem());
  float width = 0.0f;
  for (const auto& g : font.shape(utf8)) width += scale * g.x_advance;
  return width;
}

float UiRenderer::ascent(const TextStyle& style) const {
  const Font& font = font_for(style.weight);
  return style.size / static_cast<float>(font.upem()) * font.ascent_em();
}

float UiRenderer::descent(const TextStyle& style) const {
  const Font& font = font_for(style.weight);
  return style.size / static_cast<float>(font.upem()) * font.descent_em();
}

UiRenderer::InkBox UiRenderer::measure_ink(std::string_view utf8, const TextStyle& style) const {
  const Font& font = font_for(style.weight);
  const float scale = style.size / static_cast<float>(font.upem());

  InkBox box;
  float cursor_x = 0.0f;
  for (const auto& g : font.shape(utf8)) {
    const GlyphInfo& gi = font.glyph_info(g.glyph_index);
    if (!gi.is_empty) {
      const float ox = cursor_x + scale * g.x_offset, oy = scale * g.y_offset;
      const float min_x = ox + scale * gi.min_x, max_x = ox + scale * gi.max_x;
      const float min_y = oy + scale * gi.min_y, max_y = oy + scale * gi.max_y;
      if (!box.has_ink) {
        box = {.min_x = min_x, .max_x = max_x, .min_y = min_y, .max_y = max_y, .has_ink = true};
      } else {
        box.min_x = std::min(box.min_x, min_x);
        box.max_x = std::max(box.max_x, max_x);
        box.min_y = std::min(box.min_y, min_y);
        box.max_y = std::max(box.max_y, max_y);
      }
    }
    cursor_x += scale * g.x_advance;
  }
  return box;
}

void UiRenderer::text_centered(const Rect& r, std::string_view utf8, const TextStyle& style) {
  const InkBox ink = measure_ink(utf8, style);
  if (!ink.has_ink) return;  // nothing to draw, and no box to centre on
  // text() places the origin at (x, baseline) with ink extending upwards, so shifting the origin by
  // the ink midpoint puts the painted marks dead centre on both axes.
  text(r.cx() - (ink.min_x + ink.max_x) * 0.5f, r.cy() + (ink.min_y + ink.max_y) * 0.5f, utf8, style);
}

bool UiRenderer::button(const Rect& r, std::string_view label, const ButtonStyle& style) {
  const bool hover = input_.hovers(r);
  if (input_.pressed && hover) {
    active_button_ = r;
    has_active_button_ = true;
  }
  const bool held = has_active_button_ && active_button_ == r;
  const bool clicked = held && input_.released && hover;

  const glm::vec4 fill = (held && input_.down) ? style.active_fill : (hover ? style.hover_fill : style.fill);
  rect(r, fill, style.radius);
  text_centered(r, label, style.label);
  return clicked;
}

void UiRenderer::render(VkCommandBuffer cmd, uint32_t frame_index, VkImageView color_view, VkExtent2D extent) {
  const UiFrameUbo ubo{.mvp = mvp_, .viewport = {static_cast<float>(extent.width), static_cast<float>(extent.height)}};
  frame_ubos_[frame_index].upload(&ubo, sizeof(ubo));

  if (!rect_vertices_.empty())
    rect_vertex_buffers_[frame_index].upload(rect_vertices_.data(), rect_vertices_.size() * sizeof(RectVertex));
  if (!glyph_vertices_.empty())
    glyph_vertex_buffers_[frame_index].upload(glyph_vertices_.data(), glyph_vertices_.size() * sizeof(GlyphVertex));

  const VkRenderingAttachmentInfo color_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = color_view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
  };
  const VkRenderingInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {.offset = {0, 0}, .extent = extent},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment,
  };
  vk_->vkCmdBeginRendering(cmd, &rendering_info);
  const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
  const VkRect2D scissor{{0, 0}, extent};
  vk_->vkCmdSetViewport(cmd, 0, 1, &viewport);
  vk_->vkCmdSetScissor(cmd, 0, 1, &scissor);

  if (!rect_vertices_.empty()) {
    vk_->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.rect_pipeline);
    vk_->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.rect_pipeline_layout, 0, 1,
                             &descriptor_sets_[frame_index], 0, nullptr);
    const VkBuffer buf = rect_vertex_buffers_[frame_index].handle();
    const VkDeviceSize offset = 0;
    vk_->vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
    vk_->vkCmdDraw(cmd, static_cast<uint32_t>(rect_vertices_.size()), 1, 0, 0);
  }

  if (!text_runs_.empty()) {
    vk_->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.text_pipeline);
    vk_->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.text_pipeline_layout, 0, 1,
                             &descriptor_sets_[frame_index], 0, nullptr);
    const VkBuffer buf = glyph_vertex_buffers_[frame_index].handle();
    const VkDeviceSize offset = 0;
    vk_->vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
    for (const TextRun& run : text_runs_) {
      const TextPushConstants push{.foreground = {run.color.r, run.color.g, run.color.b, run.color.a}};
      vk_->vkCmdPushConstants(cmd, pipelines_.text_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
      vk_->vkCmdDraw(cmd, run.vertex_count, 1, run.vertex_offset, 0);
    }
  }

  vk_->vkCmdEndRendering(cmd);
}

void UiRenderer::destroy() noexcept {
  if (!device_) return;
  // The caller may still have frames in flight referencing these pipelines and buffers; draining
  // here keeps destruction order-independent rather than a trap for whoever copies this template.
  vk_->vkDeviceWaitIdle(device_);
  if (descriptor_pool_) vk_->vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
  pipelines_.destroy(*vk_, device_);
  descriptor_pool_ = VK_NULL_HANDLE;
}

UiRenderer::UiRenderer(UiRenderer&& other) noexcept
    : vk_(std::exchange(other.vk_, nullptr)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      atlas_(std::move(other.atlas_)),
      fonts_(std::move(other.fonts_)),
      pipelines_(std::exchange(other.pipelines_, UiPipelineSet{})),
      descriptor_pool_(std::exchange(other.descriptor_pool_, VK_NULL_HANDLE)),
      descriptor_sets_(std::exchange(other.descriptor_sets_, {})),
      frame_ubos_(std::move(other.frame_ubos_)),
      rect_vertex_buffers_(std::move(other.rect_vertex_buffers_)),
      glyph_vertex_buffers_(std::move(other.glyph_vertex_buffers_)),
      rect_vertices_(std::move(other.rect_vertices_)),
      glyph_vertices_(std::move(other.glyph_vertices_)),
      text_runs_(std::move(other.text_runs_)),
      mvp_(other.mvp_),
      extent_(other.extent_),
      input_(other.input_),
      active_button_(other.active_button_),
      has_active_button_(other.has_active_button_) {}

UiRenderer& UiRenderer::operator=(UiRenderer&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  vk_ = std::exchange(other.vk_, nullptr);
  device_ = std::exchange(other.device_, VK_NULL_HANDLE);
  atlas_ = std::move(other.atlas_);
  fonts_ = std::move(other.fonts_);
  pipelines_ = std::exchange(other.pipelines_, UiPipelineSet{});
  descriptor_pool_ = std::exchange(other.descriptor_pool_, VK_NULL_HANDLE);
  descriptor_sets_ = std::exchange(other.descriptor_sets_, {});
  frame_ubos_ = std::move(other.frame_ubos_);
  rect_vertex_buffers_ = std::move(other.rect_vertex_buffers_);
  glyph_vertex_buffers_ = std::move(other.glyph_vertex_buffers_);
  rect_vertices_ = std::move(other.rect_vertices_);
  glyph_vertices_ = std::move(other.glyph_vertices_);
  text_runs_ = std::move(other.text_runs_);
  mvp_ = other.mvp_;
  extent_ = other.extent_;
  input_ = other.input_;
  active_button_ = other.active_button_;
  has_active_button_ = other.has_active_button_;
  return *this;
}

UiRenderer::~UiRenderer() { destroy(); }

}  // namespace vkhb::ui
