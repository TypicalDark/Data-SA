#version 450
// One vertex format for fills, lines and point symbols:
//   pos  - position inside the tile, 0..1
//   ext  - extrusion in screen pixels (line half-width along the normal, symbol corner); 0 for fills
//   minz - zoom at which the vertex is fully visible; it fades in over the 0.7 zoom before
//   h    - height above the ground in metres (extruded buildings)
// The flat screen position is then tilted as geo::Camera::tilt does (3D view).
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 ext;
layout(location = 2) in vec4 color;
layout(location = 3) in float minz;
layout(location = 4) in float h;

layout(push_constant) uniform PC {
  vec2 origin;    // tile origin in flat framebuffer pixels
  float k;        // framebuffer pixels per tile unit
  float zoom;
  vec2 inv_half;  // 2 / framebuffer size
  float alpha;
  float px;       // framebuffer pixels per logical pixel
  vec2 centre;    // screen centre, framebuffer pixels
  float sin_p, cos_p;
  float dist;     // camera height over the centre, framebuffer pixels
  float zscale;   // framebuffer pixels per metre
} pc;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_tile;

void main() {
  vec2 p = pc.origin + pos * pc.k + ext * pc.px;
  float X = p.x - pc.centre.x, Y = p.y - pc.centre.y, z = h * pc.zscale;
  float Yp = Y * pc.cos_p - z * pc.sin_p, Zp = Y * pc.sin_p + z * pc.cos_p;
  float w = max(0.02, (pc.dist - Zp) / pc.dist);
  gl_Position = vec4((pc.centre * w + vec2(X, Yp)) * pc.inv_half - w, 0.0, w);
  float vis = clamp((pc.zoom - (minz - 0.7)) / 0.7, 0.0, 1.0);
  v_color = vec4(color.rgb, color.a * pc.alpha * vis);
  v_tile = pos;
}
