// Shared once-per-frame UBO for both UI pipelines (rect, text): a plain pixel-space orthographic
// projection (origin top-left, +y down, matching screen/window conventions) plus the viewport size
// hb_gpu_dilate() needs to convert its half-pixel outline dilation from clip space to pixels.
layout(set = 0, binding = 1, std140) uniform UiFrameUbo {
  mat4 u_mvp;
  vec2 u_viewport;
};
