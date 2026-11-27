// The website's layer catalogue and topics, and what is on the map now.
//
// 76 layers in nine groups plus base map toggles, as on the live map (CHIPDEF). A layer draws
// when the data packs carry it; the rest list as "needs a data pack" until their pack ships.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "style.hpp"

namespace gsa::layers {

enum class Swatch { Circle, Square, Line, Fill, Diamond, Plane };

struct Group {
  const char* id;     // grid, water, ...
  const char* name;   // tab label: Power, Water, ...
  const char* colour; // family colour for type tags
};

struct Def {
  const char* id;     // website id: plants, subs, c:pop, x:farms ...
  const char* name;
  const char* group;  // Group::id, or "base"
  Swatch swatch;
  const char* colour;
  const char* about;  // one line for the catalogue
};

struct Topic {
  const char* id;     // start, grid, vote, ...
  const char* rail;   // rail label (START, GRID ...) or empty when only on a key
  const char* name;   // Overview, Power and grid ...
  const char* key;    // keyboard key
  std::vector<const char*> layers;  // what the topic puts on the map
  const char* blurb;
};

const std::vector<Group>& groups();
const std::vector<Def>& catalogue();
const std::vector<Topic>& topics();
const Def* find(const std::string& id);
const Group* group(const std::string& id);
// Layer ids matched loosely by name or id ("substations", "subs", "power lines").
const Def* match(const std::string& text);

// Choropleths colour the municipalities; only one can be on.
bool is_choropleth(const std::string& id);

// What is drawn: layers in list order (top first), hidden ones, the focus (colour) layer, base toggles.
struct State {
  std::string topic = "start";
  std::vector<std::string> on;
  std::set<std::string> hidden;
  std::string focus;
  bool names = true, roads = true, terrain = false, buildings = false, landmarks = false;

  bool shows(const std::string& id) const;  // on and not hidden
  void add(const std::string& id);          // to the top; a choropleth replaces the other one
  void remove(const std::string& id);
  void toggle_hidden(const std::string& id);
  void clear();
  void set_topic(const std::string& topic_id, const std::set<std::string>& available);
  int count_in(const std::string& group_id) const;
};

// Which layers the current packs can draw. `packs` are tile pack ids ("admin", "grid").
std::set<std::string> available(const std::vector<std::string>& packs);

// How the renderer and labels should draw, derived from State.
struct MapStyle {
  style::Topic fill = style::Topic::Overview;  // People / Property / Elections colour the municipalities
  bool muni_borders = true;
  bool lines = false, subs = false, plants = false;
  bool names = true;
  bool operator==(const MapStyle& o) const {
    return fill == o.fill && muni_borders == o.muni_borders && lines == o.lines && subs == o.subs && plants == o.plants &&
           names == o.names;
  }
  bool operator!=(const MapStyle& o) const { return !(*this == o); }
};
MapStyle map_style(const State& s);

}  // namespace gsa::layers
