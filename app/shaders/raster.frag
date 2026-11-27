#version 450
layout(set = 0, binding = 0) uniform sampler2D img;

layout(push_constant) uniform PC {
  vec2 origin;
  float k;
  float alpha;
  vec2 inv_half;
  vec2 uv0;
  float uvk;
  float pad[3];
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  vec4 c = texture(img, v_uv);
  float a = c.a * pc.alpha;
  if (a <= 0.003) discard;
  o_color = vec4(c.rgb, a);
}
