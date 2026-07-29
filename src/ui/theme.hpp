#pragma once

// The template's default color set — a neutral dark theme.
//
// Colors stay in raw sRGB space (hex_to_srgb, not hex_to_linear): the UI renders into a UNORM
// target, so alpha blends on the stored gamma-encoded bytes the way CSS does. Blending in linear
// space would let far more of a bright background bleed through a dark translucent panel than the
// authored alpha implies.
//
// Plain functions, not namespace-scope constants: an `inline const glm::vec4` initialized by a
// runtime call is a static constructor. Replace or extend these freely.

#include "../render/color.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

namespace vkhb::ui::theme {

inline glm::vec4 rgba(uint32_t hex, float alpha = 1.0f) { return glm::vec4(vkhb::render::hex_to_srgb(hex), alpha); }

// Surfaces, darkest to lightest.
inline glm::vec4 ink900() { return rgba(0x07090c); }
inline glm::vec4 ink800() { return rgba(0x0d1117); }
inline glm::vec4 ink700() { return rgba(0x161b22); }
inline glm::vec4 ink600() { return rgba(0x21262d); }
inline glm::vec4 ink500() { return rgba(0x2d333b); }

// Text, decreasing emphasis.
inline glm::vec4 text() { return rgba(0xe6edf3); }
inline glm::vec4 text_dim() { return rgba(0x9aa7b4); }
inline glm::vec4 text_faint() { return rgba(0x6b7784); }

// Accent (interactive elements, highlights) and semantic status colors.
inline glm::vec4 accent() { return rgba(0x4c9aff); }
inline glm::vec4 accent_dim() { return rgba(0x2c5f9e); }
inline glm::vec4 success() { return rgba(0x57ab5a); }
inline glm::vec4 warning() { return rgba(0xd29922); }
inline glm::vec4 danger() { return rgba(0xe5534b); }

// Translucent fills and hairline borders (panel bodies, hovered rows, separators).
inline glm::vec4 glass() { return rgba(0x0d1117, 0.62f); }
inline glm::vec4 glass_hi() { return rgba(0x21262d, 0.72f); }
inline glm::vec4 edge() { return rgba(0x8b98a5, 0.20f); }
inline glm::vec4 edge_hi() { return rgba(0x4c9aff, 0.55f); }

// Modal chrome: full-screen dimmer behind a panel, and the panel frame's own fill.
inline glm::vec4 backdrop_dim() { return rgba(0x04060a, 0.72f); }
inline glm::vec4 frame_fill() { return rgba(0x0f141b, 0.96f); }

inline glm::vec4 with_alpha(glm::vec4 c, float a) { return {c.r, c.g, c.b, a}; }

}  // namespace vkhb::ui::theme
