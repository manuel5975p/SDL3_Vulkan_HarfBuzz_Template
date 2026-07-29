#version 450

// Rounded/circular rect primitive: one instance emits 4 CPU-computed corners (already rotated in
// screen space by the caller, if any) plus the unrotated local offset/half-size/radius the
// fragment shader needs for its signed-distance rounded-rect test.
layout(location = 0) in vec2 inPosition;   // final screen-space pixel position of this corner
layout(location = 1) in vec2 inLocal;      // offset from rect centre in the unrotated local frame
layout(location = 2) in vec2 inHalfSize;   // rect half-extent, pixels
layout(location = 3) in float inRadius;    // corner radius, pixels (0 = sharp; == min(halfSize) = circle/pill)
layout(location = 4) in vec4 inColor;      // straight (non-premultiplied) RGBA

#include "ui_common.glsl"

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec2 vHalfSize;
layout(location = 2) out float vRadius;
layout(location = 3) out vec4 vColor;

void main() {
  gl_Position = u_mvp * vec4(inPosition, 0.0, 1.0);
  vLocal = inLocal;
  vHalfSize = inHalfSize;
  vRadius = inRadius;
  vColor = inColor;
}
