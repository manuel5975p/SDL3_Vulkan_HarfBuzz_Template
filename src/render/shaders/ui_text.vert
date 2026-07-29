#version 450

// GPU glyph rendering via HarfBuzz-GPU's "Slug" encoder (third_party/harfbuzz, HB_BUILD_GPU):
// hb_gpu_draw_encode() bakes each glyph's outline into a compact per-glyph blob (quantized curve
// bands) appended to a shared atlas texel buffer; this quad just carries the per-vertex geometry
// (position/texcoord/normal/emPerPos, see ui_font.hpp) plus a constant atlas offset for the whole
// glyph. hb_gpu_dilate() (vendored, src/hb-gpu-vertex.glsl) grows the quad by half a screen pixel
// along its outward normal so anti-aliased edges never clip.
layout(location = 0) in vec2 inPosition;    // object-space (pixel) position
layout(location = 1) in vec2 inTexCoord;    // em-space atlas sample coordinate
layout(location = 2) in vec2 inNormal;      // object-space outward normal at this corner
layout(location = 3) in float inEmPerPos;   // em units per pixel (upem / font_size)
layout(location = 4) in uint inGlyphLoc;    // atlas offset, constant across one glyph's 4 corners

#include "ui_common.glsl"
#include "hb-gpu-vertex.glsl"

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) flat out uint vGlyphLoc;

void main() {
  vec2 pos = inPosition;
  vec2 tex = inTexCoord;

  // Inverse of the em-to-object linear part: object-space is a uniform scale with a y-flip
  // (position.y = cursor.y - scale*ey in ui_font.cpp), so jac = (1/scale, 0, 0, -1/scale).
  vec4 jac = vec4(inEmPerPos, 0.0, 0.0, -inEmPerPos);

  hb_gpu_dilate(pos, tex, inNormal, jac, u_mvp, u_viewport);

  gl_Position = u_mvp * vec4(pos, 0.0, 1.0);
  vTexCoord = tex;
  vGlyphLoc = inGlyphLoc;
}
