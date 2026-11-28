#include "layers.hpp"

#include <algorithm>
#include <cctype>

namespace gsa::layers {

const std::vector<Group>& groups() {
  static const std::vector<Group> g = {
      {"grid", "Power", "#ffb020"},     {"water", "Water", "#6baed6"},  {"land", "Farms", "#8bc34a"},
      {"nature", "Nature", "#3ddc84"},  {"places", "Places", "#35d0f0"}, {"move", "Travel", "#9fb3c8"},
      {"vote", "Votes", "#2e9e4f"},     {"prop", "Property", "#de4968"}, {"risk", "Hazards", "#ff7043"},
  };
  return g;
}

const std::vector<Def>& catalogue() {
  using S = Swatch;
  static const std::vector<Def> d = {
      // power
      {"plants", "Power stations", "grid", S::Circle, "#3e3e3e", "Power stations by fuel and size (OSM, Eskom)"},
      {"subs", "Substations", "grid", S::Square, "#e0302d", "Substations by voltage (OSM 24 Nov 2026)"},
      {"trafos", "Transformers", "grid", S::Square, "#2aa36b", "Transformers mapped in OSM"},
      {"lines", "Power lines", "grid", S::Line, "#b0207e", "Transmission and distribution lines by voltage"},
      {"flow", "Flow animation", "grid", S::Line, "#ffb020", "Animated direction of supply"},
      {"zones", "Service areas", "grid", S::Fill, "#ef7f1a", "Areas each substation serves (inferred)"},
      {"cct", "Cape Town network", "grid", S::Line, "#ef7f1a", "City of Cape Town electricity network"},
      {"x:supauth", "Eskom or City supply (Cape Town)", "grid", S::Fill, "#9fb3c8", "Who supplies each area"},
      {"gnet", "Mapping gaps", "grid", S::Line, "#7d8b97", "Inferred joins where OSM has gaps"},
      {"x:wind", "Wind farms", "grid", S::Circle, "#1f968a", "Wind farms and turbines"},
      {"x:splants", "Solar plants", "grid", S::Circle, "#e2a400", "Utility solar plants"},
      {"x:sembed", "Small solar arrays", "grid", S::Square, "#e2a400", "Rooftop and small arrays in OSM"},
      {"x:pvout", "Solar yield", "grid", S::Fill, "#e2a400", "Global Solar Atlas PV output"},
      {"x:ghi", "Sunshine (GHI)", "grid", S::Fill, "#f0c040", "Global horizontal irradiation"},
      {"x:roof", "Rooftop solar potential", "grid", S::Fill, "#f0c040", "Roof area and yield by suburb"},
      {"x:reea", "Renewable project applications", "grid", S::Fill, "#3ddc84", "DFFE REEA register"},
      {"x:redz", "REDZ and power corridors", "grid", S::Fill, "#ffb020", "Renewable development zones"},
      {"x:gcca", "Grid connection capacity", "grid", S::Circle, "#2aa36b", "NTCSA GCCA 2025 MW free"},
      // water
      {"rivers", "Rivers", "water", S::Line, "#2f78bf", "Rivers and streams"},
      {"bodies", "Dams and lakes", "water", S::Fill, "#4fa3e8", "Dams, lakes and reservoirs"},
      {"winfra", "Water works", "water", S::Square, "#6baed6", "Water and wastewater works"},
      {"x:rain12", "Rain, last 12 months vs normal", "water", S::Fill, "#4fa3e8", "NASA POWER rainfall anomaly"},
      {"x:rainn", "Average yearly rain", "water", S::Fill, "#4fa3e8", "Long-term mean rainfall"},
      {"x:rainm", "Rain by month, 1998 to now", "water", S::Fill, "#4fa3e8", "Monthly rainfall series"},
      {"x:tempn", "Temperature by month", "water", S::Fill, "#ff7043", "Monthly mean temperature"},
      {"x:damlvl", "Dam levels this week (DWS)", "water", S::Circle, "#1e78d2", "DWS weekly dam report"},
      {"x:wsvc", "Water and sewage works", "water", S::Square, "#6baed6", "Treatment works"},
      {"x:gdcrit", "Failing sewage works (Green Drop)", "water", S::Square, "#e0302d", "Green Drop critical works"},
      // hazards
      {"x:fire", "Fire history", "risk", S::Fill, "#ff7043", "Burnt area history"},
      {"x:flood", "Flood-prone land (modelled)", "risk", S::Fill, "#4fa3e8", "Modelled flood zones"},
      {"x:dolo", "Dolomite (sinkholes)", "risk", S::Fill, "#bfa07a", "Dolomite land"},
      {"x:quakes", "Earthquakes and tremors", "risk", S::Circle, "#ff7043", "Recorded seismic events"},
      // nature
      {"x:pa", "Protected areas", "nature", S::Fill, "#3ddc84", "SAPAD protected areas"},
      {"x:cba", "Critical Biodiversity Areas", "nature", S::Fill, "#2e9e4f", "Provincial CBA maps"},
      {"x:cbaq", "Aquatic biodiversity areas", "nature", S::Fill, "#2f78bf", "Aquatic CBAs"},
      {"x:threat", "Threatened ecosystems", "nature", S::Fill, "#e0302d", "Threatened ecosystem list"},
      {"x:wetl", "Wetlands", "nature", S::Fill, "#1f968a", "NWM5 wetlands"},
      {"x:forest", "Natural forests", "nature", S::Fill, "#2e7d32", "Natural forest patches"},
      {"x:fepa", "Freshwater priority areas", "nature", S::Fill, "#4fa3e8", "NFEPA"},
      {"x:swsa", "Strategic water source areas", "nature", S::Fill, "#6baed6", "SWSA"},
      {"x:paexp", "Protected area expansion", "nature", S::Fill, "#8bc34a", "NPAES focus areas"},
      // votes and people
      {"c:ward", "Ward winners", "vote", S::Fill, "#2e9e4f", "IEC 2026 ward winners"},
      {"c:muni", "Council control", "vote", S::Fill, "#2e9e4f", "Party that won most wards, per municipality"},
      {"c:pop", "Population density", "vote", S::Fill, "#a3195b", "People per km2, Census 2022"},
      {"c:soc", "Census 2011 indicator", "vote", S::Fill, "#9fb3c8", "Census 2011 indicators by ward"},
      {"wards", "Ward outlines", "vote", S::Line, "#3d5868", "2026 ward boundaries"},
      {"munis", "Municipal borders", "vote", S::Line, "#6f8a9c", "Municipal boundaries (MDB)"},
      {"x:wvt", "Plot values x turnout", "vote", S::Fill, "#de4968", "Roll values against turnout"},
      // property
      {"parcels", "Property plots", "prop", S::Fill, "#de4968", "Cadastral plots with roll values"},
      {"c:val", "Values by municipality", "prop", S::Fill, "#de4968", "Median market value per municipality"},
      {"x:sdf", "Development plans (SDFs)", "prop", S::Fill, "#9d6ad6", "Municipal spatial development frameworks"},
      {"x:sub", "Suburbs: people and values", "prop", S::Fill, "#fe9f6d", "Suburb population and values"},
      {"x:near", "Plots and farms near big power lines", "prop", S::Fill, "#ffb020", "Within 500 m of 132 kV+"},
      {"x:own", "State, municipal and private (roll categories)", "prop", S::Fill, "#9fb3c8", "Roll categories"},
      // farms
      {"x:farms", "Farm use", "land", S::Fill, "#8bc34a", "Crop and farm type"},
      {"x:mining", "Mining and prospecting rights", "land", S::Fill, "#bfa07a", "DMRE rights"},
      {"x:lu", "Cropland vs grazing by municipality", "land", S::Fill, "#8bc34a", "Land cover 2021 summary"},
      // places
      {"x:crime", "Crime by police precinct", "places", S::Fill, "#e0302d", "SAPS crime stats"},
      {"x:mastx", "Cell masts (exact positions)", "places", S::Diamond, "#35d0f0", "Mast positions"},
      {"x:cells", "Networks seen (OpenCelliD, estimated)", "places", S::Circle, "#35d0f0", "OpenCelliD cells"},
      {"x:masts", "OSM masts, data centres, exchanges", "places", S::Diamond, "#9fb3c8", "Telecom sites in OSM"},
      {"x:fibre", "Fibre routes (Cape Town, Limpopo)", "places", S::Line, "#35d0f0", "Fibre routes"},
      {"x:fibre2", "Home fibre by suburb", "places", S::Fill, "#35d0f0", "Fibre coverage by suburb"},
      {"x:wifi", "Free public Wi-Fi (Western Cape)", "places", S::Circle, "#35d0f0", "Public Wi-Fi sites"},
      // travel
      {"x:live", "Live planes", "move", S::Plane, "#e3eef3", "ADS-B positions"},
      {"x:ev", "EV chargers", "move", S::Square, "#3ddc84", "Open Charge Map"},
      {"x:liveships", "Live ships", "move", S::Diamond, "#4fa3e8", "AIS positions"},
      {"x:traffic", "Road traffic", "move", S::Line, "#ffb020", "Traffic counts"},
      {"x:air", "Airports", "move", S::Plane, "#9fb3c8", "OurAirports"},
      {"x:flights", "Flight routes", "move", S::Line, "#9fb3c8", "OpenSky routes 2019-21"},
      {"x:ships", "Ships and ports", "move", S::Fill, "#4fa3e8", "World Bank ship density and ports"},
      // base map
      {"streets", "Roads", "base", S::Line, "#5b4630", "Roads"},
      {"relief", "Terrain", "base", S::Fill, "#6f8a9c", "Hillshade"},
      {"labels", "Names", "base", S::Fill, "#c9d6de", "Place and area names"},
      {"buildings", "Buildings", "base", S::Fill, "#22303b", "Building footprints"},
      {"pois", "Landmarks", "base", S::Circle, "#c9d6de", "Landmarks"},
  };
  return d;
}

const std::vector<Topic>& topics() {
  static const std::vector<Topic> t = {
      {"start", "START", "Overview", "1", {"munis"}, "The country with nothing coloured. Pick a topic, search, or add layers."},
      {"grid", "GRID", "Power and grid", "2", {"plants", "subs", "trafos", "lines", "zones"},
       "Power lines by voltage, substations and power stations."},
      {"vote", "VOTE", "Votes and people", "3", {"c:ward", "c:muni", "wards", "munis"},
       "2026 ward winners and council control."},
      {"water", "WATER", "Water", "4", {"rivers", "bodies", "winfra", "x:damlvl"}, "Rivers, dams and water works."},
      {"prop", "PLOTS", "Property", "5", {"parcels", "c:val", "munis"}, "Plots and values from the valuation rolls."},
      {"land", "", "Farms and land", "6", {"x:farms", "x:mining", "munis"}, "Farm use, mining rights and land cover."},
      {"places", "", "Places", "7", {"x:crime", "x:masts", "munis"}, "Crime, masts and fibre."},
      {"move", "", "Travel", "8", {"x:air", "x:flights", "x:ships", "x:ev"}, "Airports, routes, ports and chargers."},
      {"nature", "", "Nature", "9", {"x:pa", "x:cba", "x:wetl"}, "Protected and sensitive areas."},
      {"risk", "", "Hazards", "0", {"x:flood", "x:fire", "x:dolo", "x:quakes"}, "Floods, fires, sinkholes and quakes."},
  };
  return t;
}

const Def* find(const std::string& id) {
  for (const auto& d : catalogue())
    if (id == d.id) return &d;
  return nullptr;
}

const Group* group(const std::string& id) {
  for (const auto& g : groups())
    if (id == g.id) return &g;
  return nullptr;
}

namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
}  // namespace

const Def* match(const std::string& text) {
  const std::string t = lower(text);
  if (t.empty()) return nullptr;
  if (const auto* d = find(t)) return d;
  if (const auto* d = find("x:" + t)) return d;
  if (const auto* d = find("c:" + t)) return d;
  for (const auto& d : catalogue())
    if (lower(d.name) == t) return &d;
  for (const auto& d : catalogue())
    if (lower(d.name).rfind(t, 0) == 0) return &d;
  for (const auto& d : catalogue())
    if (lower(d.name).find(t) != std::string::npos) return &d;
  return nullptr;
}

bool is_choropleth(const std::string& id) { return id.rfind("c:", 0) == 0; }

bool State::shows(const std::string& id) const {
  return std::find(on.begin(), on.end(), id) != on.end() && !hidden.count(id);
}

void State::add(const std::string& id) {
  if (is_choropleth(id)) on.erase(std::remove_if(on.begin(), on.end(), [](const auto& x) { return is_choropleth(x); }), on.end());
  on.erase(std::remove(on.begin(), on.end(), id), on.end());
  on.insert(on.begin(), id);
  hidden.erase(id);
  if (is_choropleth(id) || focus.empty()) focus = id;
}

void State::remove(const std::string& id) {
  on.erase(std::remove(on.begin(), on.end(), id), on.end());
  hidden.erase(id);
  if (focus == id) focus = on.empty() ? "" : on.front();
}

void State::toggle_hidden(const std::string& id) {
  if (!hidden.erase(id)) hidden.insert(id);
}

void State::clear() {
  on.clear();
  hidden.clear();
  focus.clear();
}

void State::set_topic(const std::string& topic_id, const std::set<std::string>& avail) {
  topic = topic_id;
  clear();
  for (const auto& t : topics()) {
    if (topic_id != t.id) continue;
    bool fill = false;
    for (const char* id : t.layers) {
      // the first choropleth the packs can draw wins
      if (is_choropleth(id)) {
        if (fill || !avail.count(id)) continue;
        fill = true;
      }
      on.emplace_back(id);
    }
  }
  focus = on.empty() ? "" : on.front();
  for (const auto& id : on)
    if (is_choropleth(id)) focus = id;
}

int State::count_in(const std::string& g) const {
  int n = 0;
  for (const auto& id : on)
    if (const auto* d = find(id); d && g == d->group) ++n;
  return n;
}

std::string mvt_name(const std::string& id) {
  std::string n = id;
  std::replace(n.begin(), n.end(), ':', '_');
  return n;
}

const char* const kVoltBins[6] = {"400 kV and up", "220 to 399", "100 to 219", "50 to 99", "under 50", "untagged"};
const char* const kTurnBins[5] = {"under 30%", "30 to 40%", "40 to 50%", "50 to 60%", "60%+"};
const char* const kProvinces[9] = {"KZN", "EC", "LIM", "GT", "WC", "NW", "MP", "FS", "NC"};

const std::vector<std::pair<std::string, std::string>>& fuels() {
  static const std::vector<std::pair<std::string, std::string>> f = {
      {"coal", "coal"},   {"gas", "gas"},         {"nuclear", "nuclear"}, {"hydro", "hydro"},
      {"solar", "solar"}, {"wind", "wind"},       {"biomass", "biomass and biogas"},
      {"battery", "battery"}, {"diesel", "diesel and oil"}, {"other", "other"}};
  return f;
}

const std::vector<std::string>& top_parties() {
  static const std::vector<std::string> p = {"ANC", "DA", "MK", "IFP", "EFF", "ActionSA", "PA", "other"};
  return p;
}

int volt_bin(double kv) { return kv <= 0 ? 5 : kv >= 400 ? 0 : kv >= 220 ? 1 : kv >= 100 ? 2 : kv >= 50 ? 3 : 4; }

int turn_bin(double pct) { return pct < 30 ? 0 : pct < 40 ? 1 : pct < 50 ? 2 : pct < 60 ? 3 : 4; }

std::string fuel_key(const std::string& fuel) {
  std::string s = fuel;
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (s == "oil") return "diesel";
  if (s == "biogas") return "biomass";
  for (const auto& [k, l] : fuels())
    if (k == s) return s;
  return "other";
}

bool pass_volt(const Filters& f, double kv) { return f.volt.empty() || f.volt.count(volt_bin(kv)); }

bool pass_fuel(const Filters& f, const std::string& fuel) { return f.fuel.empty() || f.fuel.count(fuel_key(fuel)); }

bool pass_ward(const Filters& f, const std::string& party, double turnout, const std::string& muni) {
  if (!f.party.empty()) {
    const auto& top = top_parties();
    const bool known = std::find(top.begin(), top.end(), party) != top.end() && party != "other";
    if (!f.party.count(known ? party : "other")) return false;
  }
  if (!f.turn.empty() && (turnout < 0 || !f.turn.count(turn_bin(turnout)))) return false;
  if (!f.prov.empty() && !f.prov_munis.count(muni)) return false;
  return true;
}

bool is_plot_pack(const std::string& id) { return id == "parcels" || id.rfind("parcels-", 0) == 0; }

std::string raster_pack(const std::string& id) {
  static const std::set<std::string> raster = {"relief", "x:fire", "x:flood", "x:ships", "x:farms", "x:rain12", "x:rainn",
                                             "x:pvout", "x:ghi", "x:roof"};
  if (!raster.count(id)) return {};
  return "raster-" + (id.rfind("x:", 0) == 0 ? id.substr(2) : id);
}

float raster_opacity(const std::string& pack, double z) {
  // the website's BitmapLayer opacities (dark theme)
  if (pack == "raster-relief") return z < 7.6 ? 0.9f : float(std::max(0.0, 0.9 - (z - 7.6) * 0.5));
  // the website fades the farm image out past z8 where its field outlines take over; the desktop keeps it
  // until a field pack ships
  if (pack == "raster-farms") return 0.9f;
  if (pack == "raster-ships") return 0.7f;
  if (pack == "raster-fire") return z > 10 ? 0.6f : 0.85f;
  if (pack == "raster-rain12" || pack == "raster-rainn") return z > 10 ? 0.45f : z > 8 ? 0.65f : 0.85f;
  if (pack == "raster-pvout" || pack == "raster-ghi" || pack == "raster-roof") return z > 10 ? 0.5f : 0.8f;
  return 1.0f;
}

namespace {
// layers drawn by the built-in admin and grid code, not as website extras
bool builtin(const std::string& id) {
  return id == "munis" || id == "labels" || id == "lines" || id == "subs" || id == "plants" || is_choropleth(id);
}
}  // namespace

std::set<std::string> available(const std::vector<std::string>& packs, const std::map<std::string, std::vector<std::string>>& pack_layers) {
  std::set<std::string> a = {"labels"};
  for (const auto& p : packs) {
    if (p == "admin") a.insert({"munis", "c:pop", "c:val", "c:muni"});
    if (p == "grid") a.insert({"lines", "subs", "plants"});
    if (p == "base") a.insert({"streets", "pois"});
    if (p == "buildings") a.insert("buildings");
    if (is_plot_pack(p)) a.insert("parcels");
  }
  std::set<std::string> names;
  for (const auto& [pack, ls] : pack_layers)
    if (pack.rfind("web-", 0) == 0) names.insert(ls.begin(), ls.end());
  const std::set<std::string> ids(packs.begin(), packs.end());
  for (const auto& d : catalogue())
    if (names.count(mvt_name(d.id)) || ids.count(raster_pack(d.id))) a.insert(d.id);
  if (names.count("wards")) a.insert("c:ward");
  return a;
}

std::set<std::string> packs_needed(const State& s, const std::map<std::string, std::vector<std::string>>& pack_layers) {
  std::set<std::string> want;
  for (const auto& id : s.on) {
    if (!s.shows(id)) continue;
    const std::string n = id == "c:ward" ? "wards" : mvt_name(id);
    for (const auto& [pack, ls] : pack_layers)
      if (pack.rfind("web-", 0) == 0 && std::find(ls.begin(), ls.end(), n) != ls.end()) want.insert(pack);
    if (pack_layers.count(raster_pack(id))) want.insert(raster_pack(id));
    if (id == "parcels") {
      const bool national = pack_layers.count("parcels") != 0;
      for (const auto& [pack, ls] : pack_layers)
        if (national ? pack == "parcels" : is_plot_pack(pack)) want.insert(pack);
    }
  }
  if (s.terrain && pack_layers.count("raster-relief")) want.insert("raster-relief");
  return want;
}

MapStyle map_style(const State& s) {
  MapStyle m;
  if (s.shows("c:pop")) m.fill = style::Topic::People;
  else if (s.shows("c:val")) m.fill = style::Topic::Property;
  else if (s.shows("c:muni") || s.shows("c:ward")) m.fill = style::Topic::Elections;
  m.muni_borders = true;  // municipal borders stay as orientation, like the website's base map
  m.lines = s.shows("lines");
  m.subs = s.shows("subs");
  m.plants = s.shows("plants");
  m.names = s.names;
  m.roads = s.roads;
  m.pois = s.landmarks;
  m.buildings = s.buildings;
  if (s.terrain) m.rasters.push_back("raster-relief");
  // website layers, bottom of the list first so the top of the list draws last
  for (auto it = s.on.rbegin(); it != s.on.rend(); ++it) {
    const std::string& id = *it;
    if (!s.shows(id)) continue;
    if (const auto rp = raster_pack(id); !rp.empty()) m.rasters.push_back(rp);
    if (id == "c:ward") {
      m.extra.push_back({Extra::Party, "wards", Swatch::Fill, 0});
      continue;
    }
    if (id == "parcels") {  // the plots pack: coloured by roll value
      m.extra.push_back({Extra::Plots, "parcel", Swatch::Fill, 0});
      continue;
    }
    if (builtin(id)) continue;
    const Def* d = find(id);
    if (!d) continue;
    Extra e;
    e.layer = mvt_name(id);
    e.swatch = d->swatch;
    e.rgba = style::hex(d->colour).packed();
    if (id == "wards") e.mode = Extra::Outline;
    m.extra.push_back(e);
  }
  return m;
}

}  // namespace gsa::layers
