// Turns decoded vector tiles into triangles for the map pipeline.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mvt.hpp"
#include "layers.hpp"
#include "style.hpp"

namespace gsa::mesh {

struct Vertex {
  float x, y;    // tile units 0..1
  float ex, ey;  // screen-pixel extrusion
  uint32_t rgba;
  float minz;
};
static_assert(sizeof(Vertex) == 24);

struct Mesh {
  std::vector<Vertex> fill;     // area fills
  std::vector<Vertex> lines;    // borders and network lines, drawn over fills
  std::vector<Vertex> symbols;  // point symbols, drawn last
  size_t vertex_count() const { return fill.size() + lines.size() + symbols.size(); }
};

struct Label {
  std::string text;
  double wx, wy;  // world units
  int level;      // 1 province, 3 municipality, ...
  float minz;
};

// Appends geometry of one tile to `out`. `pack` is the tile pack id ("admin", "grid").
void build(const std::string& pack, const mvt::Tile& tile, const layers::MapStyle& ms, Mesh& out);

// Label points of an admin tile, in world units. tile_ox/oy/size place the tile in the world.
void labels(const mvt::Tile& tile, double tile_ox, double tile_oy, double tile_size, std::vector<Label>& out);

// Helpers, public for tests.
void add_polygon(const mvt::Feature& f, uint32_t rgba, std::vector<Vertex>& out);
void add_line(const mvt::Ring& r, float half_width_px, uint32_t rgba, float minz, bool dashed, std::vector<Vertex>& out);
void add_square(float x, float y, float half_px, uint32_t rgba, float minz, std::vector<Vertex>& out);

}  // namespace gsa::mesh
