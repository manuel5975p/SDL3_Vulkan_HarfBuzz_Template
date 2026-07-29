#pragma once

// The one way a failed VkResult becomes an error string in this library.

#include <volk.h>

#include <cstdint>
#include <string>

namespace vkhb::render {

// "vkCreateFence (headless) failed: VK_ERROR_DEVICE_LOST". Vulkan-Headers ship no string_VkResult
// in this vendoring, so the reachable codes are spelled out and anything else falls back to its
// numeric value. pre: what nonnull.
inline std::string vk_error(const char* what, VkResult r) {
  const char* name = nullptr;
  switch (r) {
    case VK_ERROR_OUT_OF_HOST_MEMORY: name = "VK_ERROR_OUT_OF_HOST_MEMORY"; break;
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: name = "VK_ERROR_OUT_OF_DEVICE_MEMORY"; break;
    case VK_ERROR_INITIALIZATION_FAILED: name = "VK_ERROR_INITIALIZATION_FAILED"; break;
    case VK_ERROR_LAYER_NOT_PRESENT: name = "VK_ERROR_LAYER_NOT_PRESENT"; break;
    case VK_ERROR_EXTENSION_NOT_PRESENT: name = "VK_ERROR_EXTENSION_NOT_PRESENT"; break;
    case VK_ERROR_INCOMPATIBLE_DRIVER: name = "VK_ERROR_INCOMPATIBLE_DRIVER"; break;
    case VK_ERROR_FEATURE_NOT_PRESENT: name = "VK_ERROR_FEATURE_NOT_PRESENT"; break;
    case VK_ERROR_TOO_MANY_OBJECTS: name = "VK_ERROR_TOO_MANY_OBJECTS"; break;
    case VK_ERROR_DEVICE_LOST: name = "VK_ERROR_DEVICE_LOST"; break;
    case VK_ERROR_SURFACE_LOST_KHR: name = "VK_ERROR_SURFACE_LOST_KHR"; break;
    case VK_ERROR_OUT_OF_DATE_KHR: name = "VK_ERROR_OUT_OF_DATE_KHR"; break;
    default: break;
  }
  return std::string(what) + " failed: " + (name ? std::string(name) : std::to_string(static_cast<int32_t>(r)));
}

}  // namespace vkhb::render
