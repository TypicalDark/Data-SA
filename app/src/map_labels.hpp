// Turns the labels, facilities and lines of drawn tiles into label candidates.
#pragma once

#include <string>

#include <memory>
#include <vector>

#include "geo.hpp"
#include "layers.hpp"
#include "mvt.hpp"
#include "style.hpp"
#include "text/labels.hpp"

namespace gsa::map_labels {

struct Fonts {
  int regular = 0, medium = 0, mono = 0;
};

// One drawn tile: where it sits on screen (logical pixels).
// A feature an icon stands for, kept so a click can open its card.
struct FeatureRef {
  std::shared_ptr<const mvt::Tile> tile;
  std::string layer;
  size_t index = 0;
  double wx = 0, wy = 0;  // world position
  const mvt::Feature& feature() const;
};

struct TileOnScreen {
  std::shared_ptr<const mvt::Tile> tile;
  std::string pack;
  geo::TileId id;
  double ox = 0, oy = 0;  // flat screen position of the tile's top-left corner (before any tilt)
  double k = 0;           // screen pixels per tile unit
  const geo::Camera* cam = nullptr;  // tilts flat positions onto the screen (3D)
  std::vector<FeatureRef>* picks = nullptr;  // facility icons register here
};

// Zoom fade used by every layer: 0 below minz - 0.7, 1 from minz.
float fade(double zoom, double minz);

// Adds the candidates of one tile. Province and municipality names come from the admin pack;
// power stations and substations become icons with names, and the main lines carry their
// voltage along the line, when those layers are on.
void collect(const TileOnScreen& t, const layers::MapStyle& ms, double zoom, const Fonts& fonts, text::Engine& engine);

}  // namespace gsa::map_labels
