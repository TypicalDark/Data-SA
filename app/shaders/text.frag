#version 450
layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(push_constant) uniform PC {
  vec2 inv_half;
  float px;
  float on_edge;
  float unit;
  float tex_size;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_fill;
layout(location = 2) in vec4 v_halo;
layout(location = 3) in vec2 v_params;
layout(location = 0) out vec4 o_color;

void main() {
  float d = texture(atlas, v_uv).r;
  // anti-aliasing width from how many texels one screen pixel spans (more stable than fwidth(d))
  vec2 tx = vec2(length(dFdx(v_uv)), length(dFdy(v_uv))) * pc.tex_size;
  float w = max(0.5 * (tx.x + tx.y) * pc.unit * 0.75 * v_params.y, 1e-3);
  float fa = smoothstep(pc.on_edge - w, pc.on_edge + w, d) * v_fill.a;
  float ha = v_params.x > 0.0 ? smoothstep(pc.on_edge - v_params.x - w, pc.on_edge - v_params.x + w, d) * v_halo.a : 0.0;
  // fill over halo
  float a = fa + ha * (1.0 - fa);
  if (a <= 0.003) discard;
  vec3 c = (v_fill.rgb * fa + v_halo.rgb * ha * (1.0 - fa)) / a;
  o_color = vec4(c, a);
}
