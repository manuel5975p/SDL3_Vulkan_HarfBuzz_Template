#pragma once

#include <volk.h>

#include <expected>
#include <string>
#include <string_view>

namespace vkhb::render {

class Device;

// Loads compiled SPIR-V into a VkShaderModule. The caller owns the module and must destroy it
// through dev.vk().
// pre: asset_name is a logical asset name such as "shaders/ui_rect.vert.spv" (see assets/assets.hpp),
//      and the asset's size is a non-zero multiple of 4.
std::expected<VkShaderModule, std::string> load_shader_module(const Device& dev, std::string_view asset_name);

}  // namespace vkhb::render
