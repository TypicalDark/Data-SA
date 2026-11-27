#version 450
// One image tile as a quad, made from the vertex index (no vertex buffer), tilted like map.vert.
layout(push_constant) uniform PC {
  vec2 origin;    // tile top-left, flat framebuffer pixels
  float k;        // tile side, framebuffer pixels
  float alpha;
  vec2 inv_half;  // 2 / framebuffer size
  vec2 uv0;       // part of the image to show (a parent tile standing in shows one quarter, ...)
  float uvk;
  float pad[3];
  vec2 centre;
  float sin_p, cos_p;
  float dist;
} pc;

layout(location = 0) out vec2 v_uv;

void main() {
  const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
  vec2 q = c[gl_VertexIndex];
  vec2 p = pc.origin + q * pc.k;
  float X = p.x - pc.centre.x, Y = p.y - pc.centre.y;
  float w = max(0.02, (pc.dist - Y * pc.sin_p) / pc.dist);
  gl_Position = vec4((pc.centre * w + vec2(X, Y * pc.cos_p)) * pc.inv_half - w, 0.0, w);
  v_uv = pc.uv0 + q * pc.uvk;
}
