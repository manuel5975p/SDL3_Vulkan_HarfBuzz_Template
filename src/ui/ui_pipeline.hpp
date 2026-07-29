#pragma once

// Two screen-space pipelines sharing one descriptor set layout: `rect` (signed-distance rounded
// rects) and `text` (HarfBuzz-GPU glyph quads, see ui_font.hpp). Both alpha-blended and
// depth-untested, drawn in their own dynamic-rendering pass over the resolved frame (LOAD_OP_LOAD).

#include <volk.h>

#include <expected>
#include <string>

namespace vkhb::render {
class Device;
}

namespace vkhb::ui {

// Matches ui_text.frag's push_constant block exactly (field order/size).
struct TextPushConstants {
  float foreground[4] = {1, 1, 1, 1};  // straight RGBA
  float gamma = 1.0f;                  // 1.0 = off
  float stem_darkening = 1.0f;         // >0 = on
};

struct UiPipelineSet {
  VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;  // binding0=glyph atlas, binding1=frame UBO
  VkPipelineLayout rect_pipeline_layout = VK_NULL_HANDLE;        // no push constants
  VkPipelineLayout text_pipeline_layout = VK_NULL_HANDLE;        // adds the TextPushConstants range
  VkPipeline rect_pipeline = VK_NULL_HANDLE;
  VkPipeline text_pipeline = VK_NULL_HANDLE;

  void destroy(const VolkDeviceTable& vk, VkDevice device) noexcept;
};

// pre: the assets "shaders/ui_rect.{vert,frag}.spv" and "shaders/ui_text.{vert,frag}.spv" resolve
//      (see src/CMakeLists.txt's shader loop and assets/assets.hpp).
std::expected<UiPipelineSet, std::string> create_ui_pipelines(const render::Device& dev, VkFormat color_format);

}  // namespace vkhb::ui
