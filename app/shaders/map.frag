#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_tile;
layout(location = 0) out vec4 o_color;

void main() {
  // Each tile draws only inside its own square. Tiles carry a buffer of geometry from their
  // neighbours, and drawing it would double borders and alpha along every tile edge.
  if (v_tile.x < 0.0 || v_tile.y < 0.0 || v_tile.x >= 1.0 || v_tile.y >= 1.0) discard;
  if (v_color.a <= 0.003) discard;
  o_color = v_color;
}
