#include "shader.hpp"

#include "assets/assets.hpp"
#include "vk_context.hpp"
#include "vk_error.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace vkhb::render {

std::expected<VkShaderModule, std::string> load_shader_module(const Device& dev, std::string_view asset_name) {
  const std::string name(asset_name);
  auto bytes = assets::load(asset_name);
  if (!bytes) return std::unexpected(bytes.error());
  if (bytes->empty() || bytes->size() % 4 != 0)
    return std::unexpected("shader '" + name + "' has an invalid SPIR-V size");

  // Copied rather than pointed at: pCode is a const uint32_t*, and an asset backend is free to hand
  // out a span that is only byte-aligned (the embedded archive packs entries back to back).
  std::vector<uint32_t> code(bytes->size() / 4);
  std::memcpy(code.data(), bytes->data(), bytes->size());

  const VkShaderModuleCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = bytes->size(),
      .pCode = code.data(),
  };
  VkShaderModule module = VK_NULL_HANDLE;
  if (const VkResult r = dev.vk().vkCreateShaderModule(dev.handle(), &info, nullptr, &module); r != VK_SUCCESS)
    return std::unexpected(vk_error(("vkCreateShaderModule for '" + name + "'").c_str(), r));
  return module;
}

}  // namespace vkhb::render
