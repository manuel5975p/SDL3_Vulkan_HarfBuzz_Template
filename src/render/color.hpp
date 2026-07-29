#pragma once

// Hex-color (0xRRGGBB, as used throughout the JS species/location/equipment data) to linear-light
// float conversion. The renderer's lighting math (mesh.frag) and vertex colors both operate in
// linear space, with the swapchain's _SRGB image view doing the sRGB encode on write — so any hex
// color from the data tables must be decoded to linear before use, matching three.js's default
// color-managed pipeline (new THREE.Color(hex) stores linear internally too).

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace vkhb::render {

inline float srgb_to_linear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline glm::vec3 srgb_to_linear(glm::vec3 c) {
  return {srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b)};
}

// pre: hex fits 0xRRGGBB (top byte ignored).
inline glm::vec3 hex_to_srgb(uint32_t hex) {
  return {((hex >> 16) & 0xff) / 255.0f, ((hex >> 8) & 0xff) / 255.0f, (hex & 0xff) / 255.0f};
}

inline glm::vec3 hex_to_linear(uint32_t hex) { return srgb_to_linear(hex_to_srgb(hex)); }

}  // namespace vkhb::render
