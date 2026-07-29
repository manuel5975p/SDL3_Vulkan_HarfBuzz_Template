#include "ui_pipeline.hpp"

#include "../render/shader.hpp"
#include "../render/vk_context.hpp"
#include "../render/vk_error.hpp"
#include "ui_vertex.hpp"

#include <array>
#include <cstddef>

namespace vkhb::ui {

using vkhb::render::vk_error;

namespace {

std::expected<VkDescriptorSetLayout, std::string> create_descriptor_set_layout(const VolkDeviceTable& vk,
                                                                               VkDevice device) {
  const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
      {.binding = 0,
       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_VERTEX_BIT},
  }};
  const VkDescriptorSetLayoutCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
  };
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (const VkResult r = vk.vkCreateDescriptorSetLayout(device, &info, nullptr, &layout); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateDescriptorSetLayout (ui)", r));
  return layout;
}

VkPipelineColorBlendAttachmentState premultiplied_blend_attachment() {
  return {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT,
  };
}

}  // namespace

void UiPipelineSet::destroy(const VolkDeviceTable& vk, VkDevice device) noexcept {
  if (rect_pipeline) vk.vkDestroyPipeline(device, rect_pipeline, nullptr);
  if (text_pipeline) vk.vkDestroyPipeline(device, text_pipeline, nullptr);
  if (rect_pipeline_layout) vk.vkDestroyPipelineLayout(device, rect_pipeline_layout, nullptr);
  if (text_pipeline_layout) vk.vkDestroyPipelineLayout(device, text_pipeline_layout, nullptr);
  if (descriptor_set_layout) vk.vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
  rect_pipeline = text_pipeline = VK_NULL_HANDLE;
  rect_pipeline_layout = text_pipeline_layout = VK_NULL_HANDLE;
  descriptor_set_layout = VK_NULL_HANDLE;
}

std::expected<UiPipelineSet, std::string> create_ui_pipelines(const render::Device& dev, VkFormat color_format) {
  const VolkDeviceTable& vk = dev.vk();
  const VkDevice device = dev.handle();
  UiPipelineSet out;

  auto layout_result = create_descriptor_set_layout(vk, device);
  if (!layout_result) return std::unexpected(layout_result.error());
  out.descriptor_set_layout = *layout_result;

  const VkPipelineLayoutCreateInfo rect_layout_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &out.descriptor_set_layout,
  };
  if (const VkResult r = vk.vkCreatePipelineLayout(device, &rect_layout_info, nullptr, &out.rect_pipeline_layout);
      r != VK_SUCCESS) {
    out.destroy(vk, device);
    return std::unexpected(vk_error("vkCreatePipelineLayout (ui rect)", r));
  }

  const VkPushConstantRange text_push_range{
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(TextPushConstants)};
  const VkPipelineLayoutCreateInfo text_layout_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &out.descriptor_set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &text_push_range,
  };
  if (const VkResult r = vk.vkCreatePipelineLayout(device, &text_layout_info, nullptr, &out.text_pipeline_layout);
      r != VK_SUCCESS) {
    out.destroy(vk, device);
    return std::unexpected(vk_error("vkCreatePipelineLayout (ui text)", r));
  }

  auto rect_vert = render::load_shader_module(dev, "shaders/ui_rect.vert.spv");
  auto rect_frag = render::load_shader_module(dev, "shaders/ui_rect.frag.spv");
  auto text_vert = render::load_shader_module(dev, "shaders/ui_text.vert.spv");
  auto text_frag = render::load_shader_module(dev, "shaders/ui_text.frag.spv");
  const std::array<const std::expected<VkShaderModule, std::string>*, 4> all_modules{&rect_vert, &rect_frag,
                                                                                       &text_vert, &text_frag};
  for (const auto* m : all_modules) {
    if (!*m) {
      out.destroy(vk, device);
      return std::unexpected(m->error());
    }
  }
  const auto destroy_modules = [&] {
    for (const auto* m : all_modules) vk.vkDestroyShaderModule(device, **m, nullptr);
  };

  const VkPipelineInputAssemblyStateCreateInfo input_assembly{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineViewportStateCreateInfo viewport_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
  const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  const VkPipelineDynamicStateCreateInfo dynamic_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
      .pDynamicStates = dynamic_states.data(),
  };
  const VkPipelineMultisampleStateCreateInfo multisample{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const VkPipelineRasterizationStateCreateInfo rasterization{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,  // screen-space quads, winding not guaranteed either way
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  const VkPipelineDepthStencilStateCreateInfo no_depth{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };
  const auto blend_attachment = premultiplied_blend_attachment();
  const VkPipelineColorBlendStateCreateInfo color_blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
  };
  const VkPipelineRenderingCreateInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &color_format,
  };

  // --- rect pipeline ---
  {
    const std::array<VkVertexInputBindingDescription, 1> bindings{
        {{.binding = 0, .stride = sizeof(RectVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}}};
    const std::array<VkVertexInputAttributeDescription, 5> attributes{{
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(RectVertex, x)},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(RectVertex, local_x)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(RectVertex, half_w)},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(RectVertex, radius)},
        {.location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(RectVertex, r)},
    }};
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size()),
        .pVertexBindingDescriptions = bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = *rect_vert,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = *rect_frag,
         .pName = "main"},
    }};
    const VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_info,
        .stageCount = static_cast<uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &no_depth,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .layout = out.rect_pipeline_layout,
    };
    if (const VkResult r =
            vk.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &out.rect_pipeline);
        r != VK_SUCCESS) {
      destroy_modules();
      out.destroy(vk, device);
      return std::unexpected(vk_error("vkCreateGraphicsPipelines (ui rect)", r));
    }
  }

  // --- text pipeline ---
  {
    const std::array<VkVertexInputBindingDescription, 1> bindings{
        {{.binding = 0, .stride = sizeof(GlyphVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}}};
    const std::array<VkVertexInputAttributeDescription, 5> attributes{{
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(GlyphVertex, x)},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(GlyphVertex, tx)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(GlyphVertex, nx)},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(GlyphVertex, em_per_pos)},
        {.location = 4, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = offsetof(GlyphVertex, atlas_offset)},
    }};
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size()),
        .pVertexBindingDescriptions = bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = *text_vert,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = *text_frag,
         .pName = "main"},
    }};
    const VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_info,
        .stageCount = static_cast<uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &no_depth,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .layout = out.text_pipeline_layout,
    };
    if (const VkResult r =
            vk.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &out.text_pipeline);
        r != VK_SUCCESS) {
      destroy_modules();
      out.destroy(vk, device);
      return std::unexpected(vk_error("vkCreateGraphicsPipelines (ui text)", r));
    }
  }

  destroy_modules();
  return out;
}

}  // namespace vkhb::ui
