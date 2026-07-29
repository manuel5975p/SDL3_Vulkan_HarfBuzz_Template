#version 450

// hb-gpu's Slug coverage evaluator (vendored, src/hb-gpu-fragment.glsl + hb-gpu-draw-fragment.glsl)
// reads the atlas texel buffer at `hb_gpu_atlas` (bound in ui_pipeline.cpp, binding 0) and returns
// a [0,1] anti-aliased coverage for this fragment; everything below just applies foreground color,
// stem darkening (thin-stroke contrast boost at small sizes) and gamma to that coverage. Output is
// premultiplied alpha to match ui_rect.frag.
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) flat in uint vGlyphLoc;

layout(location = 0) out vec4 fragColor;

layout(push_constant) uniform UiTextPush {
  vec4 uForeground;      // straight RGBA
  float uGamma;          // 1.0 = off
  float uStemDarkening;  // >0 = on
};

#include "hb-gpu-fragment.glsl"
#include "hb-gpu-draw-fragment.glsl"

void main() {
  float cov = hb_gpu_draw(vTexCoord, vGlyphLoc);
  vec4 c = vec4(uForeground.rgb * uForeground.a, uForeground.a) * cov;

  // Apply stem darkening/gamma to edge coverage only, so interior color is unaffected.
  if (cov > 0.0 && cov < 1.0) {
    float adj = cov;
    if (uStemDarkening > 0.0) {
      float brightness = c.a > 0.0 ? dot(c.rgb, vec3(1.0 / 3.0)) / c.a : 0.0;
      adj = hb_gpu_stem_darken(adj, brightness, 1.0 / max(fwidth(vTexCoord).x, fwidth(vTexCoord).y));
    }
    if (uGamma != 1.0) adj = pow(adj, uGamma);
    c *= adj / cov;
  }

  fragColor = c;
}
