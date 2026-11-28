// Find places where ..., Rank what's in view and the spot summary, as on the website (patch_ui2).
//
// Works on the Find data pack (`/v1/data/find`, the website's data/x/find.json): every municipality
// with its outline and figures, plus substations with free capacity, dams, airports, ports, power
// stations and towns. Conditions are tested per municipality; distances run from the boundary.
#pragma once

#include <array>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gsa::find {

struct Muni {
  std::string code, name, prov;
  std::vector<std::array<double, 2>> ring;  // lon, lat
  double bb[4] = {0, 0, 0, 0};              // lon0, lat0, lon1, lat1
  double pop = -1, km2 = -1, dens = -1, med = -1, nv = -1;
  // Census 2011 (weighted by 2022 ward population), solar yield and Green Drop; -1 = no figure
  double un = -1, inc = -1, mat = -1, net = -1, inf = -1, elec = -1, wat = -1, car = -1, pv = -1, wsa = -1;
  std::string win;  // party that won most wards
  int win_wards = 0, wards = 0;
  bool contains(double lon, double lat) const;
};

// A point from one of the lists. What `a`, `b`, `t` hold depends on the list:
// cap: free MW, limit MW, area · dams: % full, last year %, river · air: big (1/0), -, IATA ·
// ports: -, -, class · st: MW, -, fuel · places: rank, -, -.
struct Site {
  double lon = 0, lat = 0;
  std::string name;
  double a = 0, b = -1;
  std::string t;
};

struct Data {
  std::vector<Muni> munis;
  std::vector<Site> cap, dams, air, ports, st, places;
  std::string dam_date;
  const Muni* muni(const std::string& code) const;
  const Muni* muni_at(double lon, double lat) const;
};
Data parse(const nlohmann::json& j);

// One condition. Which fields count depends on the kind (see defs()).
struct Rule {
  std::string k;  // densMin, densMax, popMin, valMax, valMin, party, nearDam, nearTown, nearCap, nearSt, nearAir, nearPort
  double v = 0, km = 0, pct = 0, mw = 0;
  std::string p;
  bool operator==(const Rule&) const = default;
};
nlohmann::json to_json(const std::vector<Rule>& rules);
std::vector<Rule> rules_from(const nlohmann::json& j);

struct Field {
  const char* f;  // v, km, pct, mw or p
  double def;
  const char* unit;
};
struct Def {
  const char* k;
  const char* group;  // People, Property, Elections, Water, Places, Power, Transport
  const char* label;
  std::vector<Field> fields;
};
const std::vector<Def>& defs();
const Def* def(const std::string& k);
Rule make(const std::string& k);  // with the website's starting values
const std::vector<std::string>& parties();

struct Preset {
  const char* name;
  std::vector<Rule> rules;
};
const std::vector<Preset>& presets();
const Preset* preset(const std::string& name_prefix);  // case-insensitive prefix, as "FIND solar"

double km_between(double lon0, double lat0, double lon1, double lat1);
// The nearest site the rule looks for and its distance from the municipality (0 when inside).
std::pair<const Site*, double> nearest(const Data& d, const Muni& m, const Rule& r);
bool pass(const Data& d, const Muni& m, const Rule& r);
// Codes of the municipalities that meet every rule (empty when there are no rules).
std::set<std::string> run(const Data& d, const std::vector<Rule>& rules);
// "Within 40 km of a substation with 100 MW free"
std::string text(const Rule& r);
// Why a municipality matched: one part per rule ("12 km to Apollo · R650 000").
std::string why(const Data& d, const Muni& m, const std::vector<Rule>& rules);

// The measures a municipality carries (Find both ways, Rank, Compare, colour): key, label, unit.
struct Measure {
  const char* key;    // un, inc, mat, net, inf, elec, wat, car, pv, wsa, pop, dens, med
  const char* group;  // Find group
  const char* label;  // "Unemployment rate"
  const char* unit;   // %, R a year, kWh/kWp, people, /km2, R
  double Muni::*field;
  double def_min, def_max;  // starting values for "at least" and "at most"
  bool census;               // Census 2011
};
const std::vector<Measure>& measures();
const Measure* measure(const std::string& key);
// "20.6%", "R56 431/yr", "1 613 kWh/kWp", or "–" when there is no figure
std::string fmt_measure(const Measure& m, double v);

// Compare: up to four municipalities side by side, one row per measure.
struct CompareRow {
  std::string label;
  std::vector<std::string> cells;
  int hi = -1, lo = -1;  // columns with the highest and lowest figure (-1 when fewer than 2 figures)
};
std::vector<CompareRow> compare(const std::vector<const Muni*>& ms);
std::string compare_csv(const std::vector<const Muni*>& ms);

// Rank what's in view.
struct RankDef {
  const char* k;
  const char* label;
  bool needs_data;  // false: from the drawn grid (subs, plants)
  bool ascending;
};
const std::vector<RankDef>& rank_defs();
struct RankRow {
  std::string name, value, sub;
  double x = 0;
  double lon = 0, lat = 0;
  std::string area;  // municipality code to open
  uint32_t colour = 0;
};
// Rows for a ranking from the Find data inside lon/lat bounds (subs and plants come from the map).
std::vector<RankRow> rank(const Data& d, const std::string& k, const double bounds[4]);

// The spot summary lines: (heading, text).
std::vector<std::pair<std::string, std::string>> spot(const Data& d, double lon, double lat, const std::vector<Rule>& rules);

std::string fmt_num(double v);  // 1 234 567
std::string fmt_km(double km);  // 450 m, 4.5 km, 45 km

}  // namespace gsa::find
