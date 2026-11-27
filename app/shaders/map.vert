#version 450
// One vertex format for fills, lines and point symbols:
//   pos  - position inside the tile, 0..1
//   ext  - extrusion in screen pixels (line half-width along the normal, symbol corner); 0 for fills
//   minz - zoom at which the vertex is fully visible; it fades in over the 0.7 zoom before
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 ext;
layout(location = 2) in vec4 color;
layout(location = 3) in float minz;

layout(push_constant) uniform PC {
  vec2 origin;    // tile origin in framebuffer pixels
  float k;        // framebuffer pixels per tile unit
  float zoom;
  vec2 inv_half;  // 2 / framebuffer size
  float alpha;
  float px;       // framebuffer pixels per logical pixel
} pc;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_tile;

void main() {
  vec2 p = pc.origin + pos * pc.k + ext * pc.px;
  gl_Position = vec4(p * pc.inv_half - 1.0, 0.0, 1.0);
  float vis = clamp((pc.zoom - (minz - 0.7)) / 0.7, 0.0, 1.0);
  v_color = vec4(color.rgb, color.a * pc.alpha * vis);
  v_tile = pos;
}
