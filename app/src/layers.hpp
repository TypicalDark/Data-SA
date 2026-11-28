// The website's layer catalogue and topics, and what is on the map now.
//
// 76 layers in nine groups plus base map toggles, as on the live map (CHIPDEF). A layer draws
// when the data packs carry it; the rest list as "needs a data pack" until their pack ships.
#pragma once

#include <map>
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
  bool names = true, roads = true, terrain = true, buildings = false, landmarks = true;
  bool operator==(const State&) const = default;

  bool shows(const std::string& id) const;  // on and not hidden
  void add(const std::string& id);          // to the top; a choropleth replaces the other one
  void remove(const std::string& id);
  void toggle_hidden(const std::string& id);
  void clear();
  void set_topic(const std::string& topic_id, const std::set<std::string>& available);
  int count_in(const std::string& group_id) const;
};

// The vector layer name a website layer has in the packs: ":" becomes "_" (x:pa -> x_pa).
std::string mvt_name(const std::string& id);

// Which layers the installed packs can draw. `packs` are tile pack ids ("admin", "grid",
// "web-water" ...); `pack_layers` the vector layers each pack holds (from /v1/packs).
std::set<std::string> available(const std::vector<std::string>& packs,
                                const std::map<std::string, std::vector<std::string>>& pack_layers = {});
// The website-layer packs ("web-..." and "raster-...") that hold a layer the state shows.
std::set<std::string> packs_needed(const State& s, const std::map<std::string, std::vector<std::string>>& pack_layers);

// Plot packs: the national `parcels` pack, or the data store's per-province `parcels-<prov>` packs
// (same layer and properties). When both are installed the national pack wins.
bool is_plot_pack(const std::string& id);

// The image-tile pack that draws a layer ("relief" -> "raster-relief", "x:fire" -> "raster-fire").
std::string raster_pack(const std::string& id);
// How opaque the website draws a raster layer at a zoom.
float raster_opacity(const std::string& pack, double zoom);

// A website layer drawn from a web pack.
struct Extra {
  enum Mode { Auto, Outline, Party, Plots } mode = Auto;
  std::string layer;  // vector layer name in the pack
  Swatch swatch = Swatch::Fill;
  uint32_t rgba = 0;
  bool operator==(const Extra& o) const { return mode == o.mode && layer == o.layer && swatch == o.swatch && rgba == o.rgba; }
};

// The website's Filter panel (drawFilters): grid lines by voltage, power stations by fuel, and
// ward results by party, turnout and province. What fails is dimmed, or hidden with `hide`.
struct Filters {
  std::set<int> volt;          // volt_bin of lines and substations
  std::set<std::string> fuel;  // fuel_key of power stations
  std::set<std::string> party; // ward winner, short name ("ANC") or "other"
  std::set<int> turn;          // turn_bin of ward turnout
  std::set<std::string> prov;  // province codes (KZN, EC, LIM, GT, WC, NW, MP, FS, NC)
  std::set<std::string> prov_munis;  // municipality codes in those provinces (the app fills this in)
  bool hide = false;
  bool grid_on() const { return !volt.empty() || !fuel.empty(); }
  bool vote_on() const { return !party.empty() || !turn.empty() || !prov.empty(); }
  bool operator==(const Filters&) const = default;
};
extern const char* const kVoltBins[6];
extern const char* const kTurnBins[5];
extern const char* const kProvinces[9];
const std::vector<std::pair<std::string, std::string>>& fuels();  // key, label
const std::vector<std::string>& top_parties();
int volt_bin(double kv);  // 0: 400 kV and up ... 4: under 50, 5: untagged
int turn_bin(double pct);
std::string fuel_key(const std::string& fuel);
bool pass_volt(const Filters& f, double kv);
bool pass_fuel(const Filters& f, const std::string& fuel);
bool pass_ward(const Filters& f, const std::string& party, double turnout, const std::string& muni);

// How the renderer and labels should draw, derived from State.
struct MapStyle {
  style::Topic fill = style::Topic::Overview;  // People / Property / Elections colour the municipalities
  bool muni_borders = true;
  bool lines = false, subs = false, plants = false;
  bool names = true;
  bool roads = false, pois = false;  // base map: roads, rail and land use; landmarks
  bool buildings = false;
  bool extrude = false;  // 3D: buildings stand up to their heights (the view is tilted)
  std::vector<Extra> extra;  // website layers, bottom first
  std::vector<std::string> rasters;  // image packs to draw over the fills, bottom first
  // Find: municipalities that meet every condition stay lit and the rest go dark
  bool find_on = false;
  std::set<std::string> find_pass;
  Filters filter;
  bool operator==(const MapStyle& o) const {
    return fill == o.fill && muni_borders == o.muni_borders && lines == o.lines && subs == o.subs && plants == o.plants &&
           names == o.names && roads == o.roads && pois == o.pois && buildings == o.buildings && extrude == o.extrude && extra == o.extra && rasters == o.rasters && find_on == o.find_on && find_pass == o.find_pass && filter == o.filter;
  }
  bool operator!=(const MapStyle& o) const { return !(*this == o); }
};
MapStyle map_style(const State& s);

}  // namespace gsa::layers
