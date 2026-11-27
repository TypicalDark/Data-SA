#version 450
// One image tile as a quad, made from the vertex index (no vertex buffer).
layout(push_constant) uniform PC {
  vec2 origin;    // tile top-left, framebuffer pixels
  float k;        // tile side, framebuffer pixels
  float alpha;
  vec2 inv_half;  // 2 / framebuffer size
  vec2 uv0;       // part of the image to show (a parent tile standing in shows one quarter, ...)
  float uvk;
  float pad[3];
} pc;

layout(location = 0) out vec2 v_uv;

void main() {
  const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
  vec2 q = c[gl_VertexIndex];
  gl_Position = vec4((pc.origin + q * pc.k) * pc.inv_half - 1.0, 0.0, 1.0);
  v_uv = pc.uv0 + q * pc.uvk;
}
