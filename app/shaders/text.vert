#version 450
// Distance-field text and icons. Positions arrive in logical screen pixels.
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 fill;
layout(location = 3) in vec4 halo;
layout(location = 4) in vec2 params;  // halo reach (field units), edge softness

layout(push_constant) uniform PC {
  vec2 inv_half;  // 2 / framebuffer size
  float px;       // framebuffer pixels per logical pixel
  float on_edge;  // field value on the outline, 0..1
  float unit;     // field value per texel of distance, 0..1
  float tex_size; // atlas side in texels
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_fill;
layout(location = 2) out vec4 v_halo;
layout(location = 3) out vec2 v_params;

void main() {
  gl_Position = vec4(pos * pc.px * pc.inv_half - 1.0, 0.0, 1.0);
  v_uv = uv;
  v_fill = fill;
  v_halo = halo;
  v_params = params;
}
