// Turns the labels, facilities and lines of drawn tiles into label candidates.
#pragma once

#include <string>

#include "geo.hpp"
#include "mvt.hpp"
#include "style.hpp"
#include "text/labels.hpp"

namespace gsa::map_labels {

struct Fonts {
  int regular = 0, medium = 0, mono = 0;
};

// One drawn tile: where it sits on screen (logical pixels).
struct TileOnScreen {
  const mvt::Tile* tile = nullptr;
  std::string pack;
  geo::TileId id;
  double ox = 0, oy = 0;  // screen position of the tile's top-left corner
  double k = 0;           // screen pixels per tile unit
};

// Zoom fade used by every layer: 0 below minz - 0.7, 1 from minz.
float fade(double zoom, double minz);

// Adds the candidates of one tile. Province and municipality names come from the admin pack;
// in the Energy topic, power stations and substations become icons with names and the main
// lines carry their voltage along the line.
void collect(const TileOnScreen& t, style::Topic topic, double zoom, const Fonts& fonts, text::Engine& engine);

}  // namespace gsa::map_labels
