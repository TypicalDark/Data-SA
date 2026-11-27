// Mapbox Vector Tile 2.1 decoder (layers, features, properties, geometry).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace gsa::mvt {

using Value = std::variant<std::monostate, std::string, double, int64_t, bool>;

struct Point {
  float x, y;  // tile units 0..1 (extent applied)
};
using Ring = std::vector<Point>;

enum class GeomType { Unknown = 0, Point = 1, Line = 2, Polygon = 3 };

struct Feature {
  uint64_t id = 0;
  GeomType type = GeomType::Unknown;
  std::vector<Ring> parts;  // points: one ring of points; lines: one ring per line; polygons: rings
  std::map<std::string, Value> props;

  std::optional<double> num(const std::string& k) const;
  std::string str(const std::string& k) const;
};

struct Layer {
  std::string name;
  uint32_t extent = 4096;
  std::vector<Feature> features;
};

struct Tile {
  std::vector<Layer> layers;
  const Layer* layer(const std::string& name) const;
};

// Throws std::exception on malformed input. `data` is uncompressed MVT bytes.
Tile decode(const std::string& data);

// Polygons as MVT stores them: an exterior ring (positive area in tile coordinates,
// y down) followed by its holes. Returns one vector of ring indices per polygon.
std::vector<std::vector<size_t>> group_polygons(const Feature& f);
double signed_area(const Ring& r);
bool point_in_polygon(const Feature& f, float x, float y);

}  // namespace gsa::mvt
