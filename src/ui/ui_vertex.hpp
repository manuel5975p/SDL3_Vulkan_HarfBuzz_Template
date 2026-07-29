#pragma once

// Vertex formats for the two UI pipelines (ui_pipeline.hpp/cpp), built fresh every frame by
// ui_renderer.cpp's immediate-mode rect()/text() calls — no instancing, just a growing triangle
// list per pipeline (UI draw counts are small: dozens of rects/strings per frame, not thousands).

#include <cstdint>

namespace vkhb::ui {

// One corner of a signed-distance rounded rect (see ui_rect.frag). `local`/`half_size`/`radius`
// are in the rect's own unrotated frame, so a rotated rect (e.g. a tree-connector line) still gets
// a correct SDF: the caller rotates `position` on the CPU but leaves `local` axis-aligned.
struct RectVertex {
  float x = 0, y = 0;                    // final screen-space pixel position
  float local_x = 0, local_y = 0;        // offset from rect centre, unrotated local frame
  float half_w = 0, half_h = 0;          // rect half-extent, pixels
  float radius = 0;                      // corner radius, pixels
  float r = 1, g = 1, b = 1, a = 1;      // straight RGBA
};

// One corner of a glyph quad, matching hb-gpu's demo glyph_vertex_t layout exactly (see ui_font.hpp
// / ui_text.vert): object-space position, em-space texcoord, outward normal, em-per-pixel scale,
// and the atlas offset shared by all 4 corners of one glyph.
struct GlyphVertex {
  float x = 0, y = 0;
  float tx = 0, ty = 0;
  float nx = 0, ny = 0;
  float em_per_pos = 0;
  uint32_t atlas_offset = 0;
};

}  // namespace vkhb::ui
