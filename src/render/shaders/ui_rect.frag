#version 450

// Signed-distance rounded rect, anti-aliased over one screen-space pixel via fwidth. Output is
// premultiplied alpha to match ui_text.frag, so both UI pipelines share one blend state
// (ONE, ONE_MINUS_SRC_ALPHA).
layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec2 vHalfSize;
layout(location = 2) in float vRadius;
layout(location = 3) in vec4 vColor;

layout(location = 0) out vec4 fragColor;

void main() {
  vec2 q = abs(vLocal) - vHalfSize + vRadius;
  float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - vRadius;
  float aa = max(fwidth(d), 1e-4);
  float alpha = clamp(0.5 - d / aa, 0.0, 1.0) * vColor.a;
  fragColor = vec4(vColor.rgb * alpha, alpha);
}
