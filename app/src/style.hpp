// Topics and colours, following the v7 UI prototype (A2 Control Room look):
// seven general topics, one colour (focus) layer, everything else in greys,
// at most three hue families on screen.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mvt.hpp"

namespace gsa::style {

struct Rgba {
  uint8_t r = 0, g = 0, b = 0, a = 255;
  uint32_t packed() const { return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24; }
};
Rgba hex(const char* h, float alpha = 1.0f);

enum class Topic { Overview = 0, People, Property, Water, Energy, Elections, Transport, Count };

struct TopicInfo {
  Topic id;
  const char* name;
  const char* key;    // keyboard shortcut
  const char* swatch; // hex
  const char* blurb;
  const char* pack_needed;  // empty when the dev pack covers it
};
const std::array<TopicInfo, size_t(Topic::Count)>& topics();
const TopicInfo& info(Topic t);

// Colour for a municipality feature in a topic; empty means "no data" (drawn hatched grey).
std::optional<Rgba> muni_fill(Topic t, const mvt::Feature& f);
Rgba land();
// The website's base map, dark theme. Land use by class (res, ind, park ...; alpha 0 = not drawn);
// the overview draws unknown classes as nature, the regional tiles leave them out.
Rgba land_use(const std::string& c, bool overview);
// Road class 0 motorway, 1-2 major, 3-7 smaller: colour (or casing), half width in pixels and
// the zoom a class is fully shown from.
Rgba road(int r, bool casing);
float road_half_width(int r, bool overview);
float road_minz(int r, bool overview);
// The website's market value bands (MV_BANDS), rand.
Rgba value_band(double rand, float alpha = 1.0f);
// Landmark colour by kind (hospital, school, police ...).
Rgba poi(const std::string& k);
Rgba background();
Rgba muni_border();
Rgba prov_border();
Rgba accent();

Rgba volt(double kv);               // power line / substation colour by voltage
Rgba plant(const std::string& fuel);
Rgba party(const std::string& short_name, float alpha = 1.0f);  // ANC, DA, ... Other
float line_width_px(double kv, bool focus);
bool line_visible(double kv, double zoom);  // readable-zoom gates from the prototype

struct LegendItem {
  std::string label;
  Rgba colour;
};
struct Legend {
  std::string title;
  std::vector<LegendItem> items;
  std::string note;
};
Legend legend(Topic t);

}  // namespace gsa::style
