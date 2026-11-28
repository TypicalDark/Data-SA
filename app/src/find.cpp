#include "find.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "style.hpp"

namespace gsa::find {

using nlohmann::json;

namespace {

double num(const json& j, const char* k, double def = -1) {
  auto it = j.find(k);
  return it != j.end() && it->is_number() ? it->get<double>() : def;
}
std::string str(const json& j, size_t i) { return i < j.size() && j[i].is_string() ? j[i].get<std::string>() : std::string(); }
double at(const json& j, size_t i, double def = -1) { return i < j.size() && j[i].is_number() ? j[i].get<double>() : def; }

std::vector<Site> sites(const json& j, const char* key, int lon_i, int name_i, int a_i, int b_i, int t_i) {
  std::vector<Site> out;
  if (!j.contains(key) || !j[key].is_array()) return out;
  for (const auto& r : j[key]) {
    if (!r.is_array()) continue;
    Site s;
    s.lon = at(r, size_t(lon_i), 0);
    s.lat = at(r, size_t(lon_i) + 1, 0);
    s.name = str(r, size_t(name_i));
    if (a_i >= 0) s.a = at(r, size_t(a_i), 0);
    if (b_i >= 0) s.b = at(r, size_t(b_i), -1);
    if (t_i >= 0) s.t = r.size() > size_t(t_i) && r[size_t(t_i)].is_number() ? std::to_string(r[size_t(t_i)].get<int>())
                                                                             : str(r, size_t(t_i));
    out.push_back(std::move(s));
  }
  return out;
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool in_bounds(double lon, double lat, const double b[4]) { return lon >= b[0] && lon <= b[2] && lat >= b[1] && lat <= b[3]; }

const std::vector<Site>& sites_for(const Data& d, const Rule& r, std::vector<Site>& tmp) {
  if (r.k == "nearDam") {
    tmp.clear();
    for (const auto& s : d.dams)
      if (s.a >= r.pct) tmp.push_back(s);
    return tmp;
  }
  if (r.k == "nearCap") {
    tmp.clear();
    for (const auto& s : d.cap)
      if (s.a >= r.mw) tmp.push_back(s);
    return tmp;
  }
  if (r.k == "nearTown") return d.places;
  if (r.k == "nearSt") return d.st;
  if (r.k == "nearAir") return d.air;
  return d.ports;
}

}  // namespace

std::string fmt_num(double v) {
  if (!std::isfinite(v)) return "\xe2\x80\x93";
  const long long n = std::llround(v);
  std::string s = std::to_string(n < 0 ? -n : n), out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i && (s.size() - i) % 3 == 0) out += ' ';
    out += s[i];
  }
  return n < 0 ? "-" + out : out;
}

std::string fmt_km(double km) {
  char b[32];
  if (km < 1) std::snprintf(b, sizeof b, "%d m", int(std::lround(km * 1000)));
  else if (km < 10) std::snprintf(b, sizeof b, "%.1f km", km);
  else std::snprintf(b, sizeof b, "%d km", int(std::lround(km)));
  return b;
}

bool Muni::contains(double lon, double lat) const {
  if (lon < bb[0] || lon > bb[2] || lat < bb[1] || lat > bb[3]) return false;
  bool in = false;
  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const auto& a = ring[i];
    const auto& b = ring[j];
    if ((a[1] > lat) != (b[1] > lat) && lon < (b[0] - a[0]) * (lat - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
  }
  return in;
}

const Muni* Data::muni(const std::string& code) const {
  for (const auto& m : munis)
    if (m.code == code) return &m;
  return nullptr;
}

const Muni* Data::muni_at(double lon, double lat) const {
  for (const auto& m : munis)
    if (m.contains(lon, lat)) return &m;
  return nullptr;
}

Data parse(const json& j) {
  Data d;
  for (const auto& m : j.value("munis", json::array())) {
    Muni u;
    u.code = m.value("c", "");
    u.name = m.value("n", "");
    u.prov = m.value("p", "");
    u.bb[0] = u.bb[1] = 1e9;
    u.bb[2] = u.bb[3] = -1e9;
    for (const auto& p : m.value("r", json::array())) {
      if (!p.is_array() || p.size() < 2) continue;
      const double x = p[0].get<double>(), y = p[1].get<double>();
      u.ring.push_back({x, y});
      u.bb[0] = std::min(u.bb[0], x);
      u.bb[1] = std::min(u.bb[1], y);
      u.bb[2] = std::max(u.bb[2], x);
      u.bb[3] = std::max(u.bb[3], y);
    }
    if (u.ring.empty()) continue;
    u.pop = num(m, "pop");
    u.km2 = num(m, "km2");
    u.med = num(m, "med");
    u.nv = num(m, "nv");
    if (u.pop >= 0 && u.km2 > 0) u.dens = u.pop / u.km2;
    for (const auto& ms : measures())
      if (ms.census || std::string(ms.key) == "pv" || std::string(ms.key) == "wsa") u.*ms.field = num(m, ms.key);
    if (m.contains("win") && m["win"].is_array() && m["win"].size() >= 3) {
      u.win = str(m["win"], 0);
      u.win_wards = int(at(m["win"], 1, 0));
      u.wards = int(at(m["win"], 2, 0));
    }
    d.munis.push_back(std::move(u));
  }
  // column layouts from the website's load()
  d.cap = sites(j, "cap", 0, 2, 3, 4, 7);
  d.dams = sites(j, "dams", 0, 2, 3, 5, 6);
  d.air = sites(j, "air", 0, 2, -1, -1, 3);
  if (j.contains("air"))
    for (size_t i = 0; i < d.air.size() && i < j["air"].size(); ++i) d.air[i].a = at(j["air"][i], 4, 0) == 2 ? 1 : 0;
  d.ports = sites(j, "ports", 0, 2, -1, -1, 3);
  d.st = sites(j, "st", 0, 3, 2, -1, 4);
  // places are [name, lon, lat, rank]
  for (const auto& r : j.value("places", json::array())) {
    if (!r.is_array() || r.size() < 3) continue;
    d.places.push_back({at(r, 1, 0), at(r, 2, 0), str(r, 0), at(r, 3, 0), -1, ""});
  }
  d.dam_date = j.value("damdate", "");
  return d;
}

json to_json(const std::vector<Rule>& rules) {
  json a = json::array();
  for (const auto& r : rules) {
    json o{{"k", r.k}};
    if (const Def* d = def(r.k))
      for (const auto& f : d->fields) {
        const std::string n = f.f;
        if (n == "p") o["p"] = r.p;
        else o[n] = n == "v" ? r.v : n == "km" ? r.km : n == "pct" ? r.pct : r.mw;
      }
    a.push_back(o);
  }
  return a;
}

std::vector<Rule> rules_from(const json& j) {
  std::vector<Rule> out;
  if (!j.is_array()) return out;
  for (const auto& o : j) {
    if (!o.is_object() || !def(o.value("k", ""))) continue;
    Rule r = make(o["k"]);
    r.v = o.value("v", r.v);
    r.km = o.value("km", r.km);
    r.pct = o.value("pct", r.pct);
    r.mw = o.value("mw", r.mw);
    r.p = o.value("p", r.p);
    out.push_back(r);
  }
  return out;
}

const std::vector<Measure>& measures() {
  static const std::vector<Measure> m = {
      {"pop", "People", "People", "people", &Muni::pop, 100000, 50000, false},
      {"dens", "People", "People per km\xc2\xb2", "/km\xc2\xb2", &Muni::dens, 100, 50, false},
      {"med", "Property", "Median property value", "R", &Muni::med, 1000000, 800000, false},
      {"un", "Jobs and income", "Unemployment rate", "%", &Muni::un, 40, 25, true},
      {"inc", "Jobs and income", "Median household income", "R a year", &Muni::inc, 40000, 30000, true},
      {"mat", "Education", "Adults with matric or more", "%", &Muni::mat, 40, 30, true},
      {"net", "Services", "Homes with internet", "%", &Muni::net, 10, 5, true},
      {"elec", "Services", "Homes with electric lighting", "%", &Muni::elec, 90, 80, true},
      {"inf", "Services", "Homes in informal dwellings", "%", &Muni::inf, 20, 10, true},
      {"wat", "Services", "Homes with piped water inside", "%", &Muni::wat, 60, 40, true},
      {"car", "Services", "Homes with a car", "%", &Muni::car, 40, 20, true},
      {"pv", "Power", "Solar yield", "kWh/kWp", &Muni::pv, 1700, 1600, false},
      {"wsa", "Water", "Green Drop wastewater score", "%", &Muni::wsa, 50, 30, false},
  };
  return m;
}

const Measure* measure(const std::string& key) {
  for (const auto& m : measures())
    if (key == m.key) return &m;
  return nullptr;
}

std::string fmt_measure(const Measure& m, double v) {
  if (v < 0 || !std::isfinite(v)) return "\xe2\x80\x93";
  const std::string u = m.unit;
  if (u == "%") {
    char b[24];
    std::snprintf(b, sizeof b, "%.1f%%", v);
    std::string s = b;
    if (s.size() > 3 && s.compare(s.size() - 3, 3, ".0%") == 0) s.erase(s.size() - 3, 2);
    return s;
  }
  if (u == "R") return "R" + fmt_num(v);
  if (u == "R a year") return "R" + fmt_num(v) + "/yr";
  if (u == "people") return fmt_num(v);
  return fmt_num(v) + " " + u;
}

namespace {
// Which way is good: fewer jobless and fewer informal homes are better; size, density and price are neither.
int better_dir(const std::string& k) {
  if (k == "un" || k == "inf") return -1;
  if (k == "pop" || k == "dens" || k == "med") return 0;
  return 1;
}
}  // namespace

std::vector<CompareRow> compare(const std::vector<const Muni*>& ms) {
  std::vector<CompareRow> out;
  auto row = [&](const std::string& label, auto value, auto text, int better) {
    CompareRow r;
    r.label = label;
    r.better = better;
    double hi = -1e300, lo = 1e300;
    int n = 0;
    for (size_t i = 0; i < ms.size(); ++i) {
      r.cells.push_back(text(*ms[i]));
      const double v = value(*ms[i]);
      if (v < 0) continue;
      ++n;
      if (v > hi) hi = v, r.hi = int(i);
      if (v < lo) lo = v, r.lo = int(i);
    }
    if (n < 2 || hi == lo) r.hi = r.lo = -1;
    out.push_back(std::move(r));
  };
  for (const auto& m : measures()) {
    if (std::string(m.key) == "wsa") continue;  // not in the national file yet
    row(m.label, [&](const Muni& u) { return u.*m.field; },
        [&](const Muni& u) { return fmt_measure(m, u.*m.field); }, better_dir(m.key));
    if (std::string(m.key) == "med")
      row("Most wards won", [](const Muni&) { return -1.0; }, [](const Muni& u) {
        return u.win.empty() ? std::string("\xe2\x80\x93") : u.win + " " + std::to_string(u.win_wards) + "/" + std::to_string(u.wards);
      }, 0);
  }
  return out;
}

std::string compare_csv(const std::vector<const Muni*>& ms) {
  auto q = [](const std::string& v) {
    if (v.find_first_of("\",") == std::string::npos) return v;
    std::string o = "\"";
    for (char ch : v) o += ch == '"' ? std::string("\"\"") : std::string(1, ch);
    return o + "\"";
  };
  std::string s = "measure";
  for (const auto* m : ms) s += "," + q(m->name);
  s += "\n";
  for (const auto& r : compare(ms)) {
    s += q(r.label);
    for (const auto& c : r.cells) s += "," + q(c == "\xe2\x80\x93" ? "" : c);
    s += "\n";
  }
  return s;
}

const std::vector<Def>& defs() {
  static const std::vector<Def> d = [] {
    std::vector<Def> v = {
      {"densMin", "People", "People per km\xc2\xb2, at least", {{"v", 100, "/km\xc2\xb2"}}},
      {"densMax", "People", "People per km\xc2\xb2, at most", {{"v", 50, "/km\xc2\xb2"}}},
      {"popMin", "People", "People living there, at least", {{"v", 100000, "people"}}},
      {"valMax", "Property", "Median property value, at most", {{"v", 800000, "R"}}},
      {"valMin", "Property", "Median property value, at least", {{"v", 1000000, "R"}}},
      {"party", "Elections", "Most wards won by", {{"p", 0, ""}}},
      {"nearDam", "Water", "Within km of a dam at least % full", {{"km", 30, "km"}, {"pct", 50, "%"}}},
      {"nearTown", "Places", "Within km of a town", {{"km", 25, "km"}}},
      {"nearCap", "Power", "Within km of a substation with MW free", {{"km", 40, "km"}, {"mw", 100, "MW"}}},
      {"nearSt", "Power", "Within km of a power station", {{"km", 30, "km"}}},
      {"nearAir", "Transport", "Within km of an airport", {{"km", 60, "km"}}},
      {"nearPort", "Transport", "Within km of a port", {{"km", 150, "km"}}},
    };
    // every Census measure, solar yield and Green Drop both ways ("unMin", "unMax", ...)
    static std::vector<std::string> labels;  // Def holds const char*
    labels.reserve(64);
    for (const auto& m : measures()) {
      const std::string k = m.key;
      if (k == "pop" || k == "dens" || k == "med") continue;
      for (int hi = 0; hi < 2; ++hi) {
        if ((k == "pv" || k == "wsa") && hi) continue;  // only "at least" makes sense
        labels.push_back(k + (hi ? "Max" : "Min"));
        const char* key = labels.back().c_str();
        labels.push_back(std::string(m.label) + (hi ? ", at most" : ", at least"));
        v.push_back({key, m.group, labels.back().c_str(), {{"v", hi ? m.def_max : m.def_min, m.unit}}});
      }
    }
    return v;
  }();
  return d;
}

// "unMin" -> (measure un, at least)
static std::pair<const Measure*, bool> measure_rule(const std::string& k) {
  if (k.size() < 4) return {nullptr, false};
  const std::string tail = k.substr(k.size() - 3), key = k.substr(0, k.size() - 3);
  if (tail != "Min" && tail != "Max") return {nullptr, false};
  const Measure* m = measure(key);
  if (!m || (!m->census && key != "pv" && key != "wsa")) return {nullptr, false};
  return {m, tail == "Max"};
}

const Def* def(const std::string& k) {
  for (const auto& d : defs())
    if (k == d.k) return &d;
  return nullptr;
}

Rule make(const std::string& k) {
  Rule r;
  r.k = k;
  if (const Def* d = def(k))
    for (const auto& f : d->fields) {
      const std::string n = f.f;
      if (n == "v") r.v = f.def;
      else if (n == "km") r.km = f.def;
      else if (n == "pct") r.pct = f.def;
      else if (n == "mw") r.mw = f.def;
      else if (n == "p") r.p = "ANC";
    }
  return r;
}

const std::vector<std::string>& parties() {
  static const std::vector<std::string> p = {"ANC", "DA", "MK", "IFP", "EFF", "ActionSA", "PA"};
  return p;
}

const std::vector<Preset>& presets() {
  auto R = [](const char* k, double a, double b = 0, double c = 0) {
    Rule r = make(k);
    const std::string s = k;
    if (s == "nearCap") r.km = a, r.mw = b;
    else if (s == "nearDam") r.km = a, r.pct = b;
    else if (s.rfind("near", 0) == 0) r.km = a;
    else r.v = a;
    (void)c;
    return r;
  };
  static const std::vector<Preset> p = {
      {"Solar farm", {R("nearCap", 40, 100)}},
      {"Data centre", {R("nearCap", 15, 20), R("densMin", 300), R("nearAir", 60)}},
      {"Warehouse near a port", {R("nearPort", 120), R("nearTown", 20)}},
      {"Affordable, busy towns", {R("valMax", 600000), R("densMin", 100)}},
      {"Water-secure areas", {R("nearDam", 30, 75)}},
      {"Remote-work town", {R("netMin", 12), R("nearAir", 80), R("valMax", 1500000)}},
      {"Solar with grid room", {R("pvMin", 1800), R("nearCap", 40, 50)}},
      {"Skilled, working towns", {R("matMin", 40), R("unMax", 30)}},
      {"Service backlog", {R("infMin", 20), R("watMax", 40)}},
  };
  return p;
}

const Preset* preset(const std::string& name_prefix) {
  const std::string q = lower(name_prefix);
  if (q.empty()) return nullptr;
  for (const auto& p : presets())
    if (lower(p.name).rfind(q, 0) == 0) return &p;
  return nullptr;
}

double km_between(double lon0, double lat0, double lon1, double lat1) {
  // the website's equirectangular distance (good to well under 1% at these ranges)
  constexpr double R = 6371, r = 3.14159265358979323846 / 180;
  const double x = (lon1 - lon0) * r * std::cos((lat0 + lat1) / 2 * r), y = (lat1 - lat0) * r;
  return R * std::hypot(x, y);
}

std::pair<const Site*, double> nearest(const Data& d, const Muni& m, const Rule& r) {
  std::vector<Site> tmp;
  const auto& fs = sites_for(d, r, tmp);
  const double dg = r.km / 100, cy = (m.bb[1] + m.bb[3]) / 2, kx = std::cos(cy * 3.14159265358979323846 / 180);
  const Site* best = nullptr;
  double bd = 1e9;
  for (const auto& o : fs) {
    if (o.lon < m.bb[0] - dg / kx || o.lon > m.bb[2] + dg / kx || o.lat < m.bb[1] - dg || o.lat > m.bb[3] + dg) continue;
    if (m.contains(o.lon, o.lat)) {
      bd = 0;
      best = &o;
      break;
    }
    for (const auto& p : m.ring) {
      const double dd = km_between(o.lon, o.lat, p[0], p[1]);
      if (dd < bd) {
        bd = dd;
        best = &o;
      }
    }
  }
  // `tmp` holds filtered copies: hand back the original
  if (best && !tmp.empty()) {
    const auto& all = r.k == "nearDam" ? d.dams : d.cap;
    for (const auto& o : all)
      if (o.name == best->name && o.lon == best->lon && o.lat == best->lat) return {&o, bd};
  }
  return {best, bd};
}

bool pass(const Data& d, const Muni& m, const Rule& r) {
  if (r.k == "densMin") return m.dens >= 0 && m.dens >= r.v;
  if (r.k == "densMax") return m.dens >= 0 && m.dens <= r.v;
  if (r.k == "popMin") return m.pop >= r.v;
  if (r.k == "valMax") return m.med > 0 && m.med <= r.v;
  if (r.k == "valMin") return m.med > 0 && m.med >= r.v;
  if (r.k == "party") return !m.win.empty() && m.win == r.p;
  if (auto [ms, at_most] = measure_rule(r.k); ms) {
    const double v = m.*ms->field;
    return v >= 0 && (at_most ? v <= r.v : v >= r.v);
  }
  if (!def(r.k)) return true;
  const auto [o, dist] = nearest(d, m, r);
  return o && dist <= r.km;
}

std::set<std::string> run(const Data& d, const std::vector<Rule>& rules) {
  std::set<std::string> out;
  if (rules.empty()) return out;
  for (const auto& m : d.munis)
    if (std::all_of(rules.begin(), rules.end(), [&](const Rule& r) { return pass(d, m, r); })) out.insert(m.code);
  return out;
}

std::string text(const Rule& r) {
  const std::string km = fmt_num(r.km) + " km";
  if (r.k == "nearDam") return "Within " + km + " of a dam at least " + fmt_num(r.pct) + "% full";
  if (r.k == "nearCap") return "Within " + km + " of a substation with " + fmt_num(r.mw) + " MW free";
  if (r.k == "nearTown") return "Within " + km + " of a town";
  if (r.k == "nearSt") return "Within " + km + " of a power station";
  if (r.k == "nearAir") return "Within " + km + " of an airport";
  if (r.k == "nearPort") return "Within " + km + " of a port";
  if (r.k == "party") return "Most wards won by " + r.p;
  const Def* d = def(r.k);
  if (!d) return r.k;
  if (auto [ms, at_most] = measure_rule(r.k); ms) return std::string(d->label) + " " + fmt_measure(*ms, r.v);
  const std::string unit = d->fields[0].unit;
  if (unit == "R") return std::string(d->label) + " R" + fmt_num(r.v);
  return std::string(d->label) + " " + fmt_num(r.v) + (unit == "people" ? "" : " " + unit);
}

std::string why(const Data& d, const Muni& m, const std::vector<Rule>& rules) {
  std::string out;
  for (const auto& r : rules) {
    std::string part;
    if (r.k == "densMin" || r.k == "densMax") part = m.dens >= 0 ? fmt_num(m.dens) + " /km\xc2\xb2" : "no figure";
    else if (r.k == "popMin") part = fmt_num(m.pop) + " people";
    else if (r.k == "valMin" || r.k == "valMax") part = m.med > 0 ? "R" + fmt_num(m.med) : "no roll data";
    else if (auto [ms, at_most] = measure_rule(r.k); ms) {
      std::string l = ms->label;
      l[0] = char(std::tolower(static_cast<unsigned char>(l[0])));
      part = m.*ms->field >= 0 ? l + " " + fmt_measure(*ms, m.*ms->field) : "no figure";
    } else if (r.k == "party")
      part = m.win.empty() ? "no result" : m.win + " " + std::to_string(m.win_wards) + "/" + std::to_string(m.wards);
    else {
      const auto [o, dist] = nearest(d, m, r);
      // say what the place is: "Jupiter substation inside", "12 km to Vaal Dam"
      const std::string noun = r.k == "nearCap" ? "substation" : r.k == "nearSt" ? "power station" : r.k == "nearAir" ? "airport" : "";
      std::string nm = o ? o->name : "", low = nm;
      std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
      if (!noun.empty() && low.find(noun.substr(noun.rfind(' ') + 1)) == std::string::npos) nm += " " + noun;
      part = !o ? "none near" : dist < 1 ? nm + " inside" : std::to_string(int(std::lround(dist))) + " km to " + nm;
    }
    if (!out.empty()) out += " \xc2\xb7 ";
    out += part;
  }
  return out;
}

const std::vector<RankDef>& rank_defs() {
  static const std::vector<RankDef> r = {
      {"subs", "Substations, by voltage", false, false},
      {"plants", "Power stations, by MW", false, false},
      {"cap", "Grid capacity free (NTCSA 2025), MW", true, false},
      {"dams", "Dams, lowest first", true, true},
      {"mval", "Municipalities, by median value", true, false},
      {"mpop", "Municipalities, by people", true, false},
      {"mun", "Municipalities, by unemployment (Census 2011)", true, false},
      {"minc", "Municipalities, by household income (Census 2011)", true, false},
      {"mmat", "Municipalities, by adults with matric (Census 2011)", true, false},
      {"mnet", "Municipalities, by homes with internet (Census 2011)", true, false},
      {"minf", "Municipalities, by informal dwellings (Census 2011)", true, false},
      {"mpv", "Municipalities, by solar yield", true, false},
      {"air", "Airports", true, false},
  };
  return r;
}

std::vector<RankRow> rank(const Data& d, const std::string& k, const double b[4]) {
  std::vector<RankRow> rows;
  auto c = [](const char* hex) { return style::hex(hex).packed(); };
  if (k == "cap") {
    for (const auto& o : d.cap) {
      if (!in_bounds(o.lon, o.lat, b)) continue;
      rows.push_back({o.name, fmt_num(o.a) + " MW", o.t + " \xc2\xb7 limit " + fmt_num(o.b) + " MW", o.a, o.lon, o.lat, "",
                      c(o.a >= 100 ? "#2aa36b" : o.a > 0 ? "#c9a20a" : "#e0302d")});
    }
  } else if (k == "dams") {
    for (const auto& o : d.dams) {
      if (!in_bounds(o.lon, o.lat, b)) continue;
      std::string s = o.t;
      if (o.b >= 0) s += (s.empty() ? "" : " \xc2\xb7 ") + std::string("last year ") + fmt_num(o.b) + "%";
      rows.push_back({o.name, fmt_num(o.a) + "%", s, o.a, o.lon, o.lat, "",
                      c(o.a < 40 ? "#e0302d" : o.a < 70 ? "#c9a20a" : "#1e78d2")});
    }
  } else if (k == "mval" || k == "mpop") {
    for (const auto& m : d.munis) {
      if (m.bb[2] < b[0] || m.bb[0] > b[2] || m.bb[3] < b[1] || m.bb[1] > b[3]) continue;
      RankRow r;
      r.name = m.name;
      r.area = m.code;
      r.lon = (m.bb[0] + m.bb[2]) / 2;
      r.lat = (m.bb[1] + m.bb[3]) / 2;
      if (k == "mval") {
        if (m.med <= 0) continue;
        r.x = m.med;
        r.value = "R" + fmt_num(m.med);
        r.sub = m.prov + " \xc2\xb7 " + fmt_num(m.nv) + " properties";
        r.colour = c("#de4968");
      } else {
        r.x = std::max(0.0, m.pop);
        r.value = fmt_num(m.pop);
        r.sub = m.prov + (m.dens >= 0 ? " \xc2\xb7 " + fmt_num(m.dens) + " per km\xc2\xb2" : "");
        r.colour = c("#c98bd0");
      }
      rows.push_back(r);
    }
  } else if (k.size() > 1 && k[0] == 'm' && measure(k.substr(1)) && k != "mpop") {
    const Measure& ms = *measure(k.substr(1));
    static const std::map<std::string, const char*> colours = {{"un", "#e0602a"}, {"inc", "#2aa36b"}, {"mat", "#8a7cf0"},
                                                               {"net", "#35d0f0"}, {"inf", "#c9a20a"}, {"pv", "#e2a400"}};
    const auto ci = colours.find(ms.key);
    for (const auto& m : d.munis) {
      if (m.bb[2] < b[0] || m.bb[0] > b[2] || m.bb[3] < b[1] || m.bb[1] > b[3]) continue;
      const double v = m.*ms.field;
      if (v < 0) continue;
      RankRow r;
      r.name = m.name;
      r.area = m.code;
      r.lon = (m.bb[0] + m.bb[2]) / 2;
      r.lat = (m.bb[1] + m.bb[3]) / 2;
      r.x = v;
      r.value = fmt_measure(ms, v);
      r.sub = m.prov + " \xc2\xb7 " + fmt_num(m.pop) + " people";
      r.colour = c(ci != colours.end() ? ci->second : "#9fb3c8");
      rows.push_back(r);
    }
  } else if (k == "air") {
    for (const auto& o : d.air) {
      if (!in_bounds(o.lon, o.lat, b)) continue;
      rows.push_back({o.name, o.t, o.a > 0 ? "Large" : "Medium", o.a > 0 ? 2.0 : 1.0, o.lon, o.lat, "", c("#e6edf3")});
    }
  }
  bool asc = false;
  for (const auto& r : rank_defs())
    if (k == r.k) asc = r.ascending;
  std::stable_sort(rows.begin(), rows.end(), [&](const RankRow& a, const RankRow& c2) { return asc ? a.x < c2.x : a.x > c2.x; });
  return rows;
}

std::vector<std::pair<std::string, std::string>> spot(const Data& d, double lon, double lat, const std::vector<Rule>& rules) {
  std::vector<std::pair<std::string, std::string>> L;
  const Muni* m = d.muni_at(lon, lat);
  if (m) {
    std::string s = m->name + ": " + fmt_num(m->pop) + " people";
    if (m->dens >= 0) s += " \xc2\xb7 " + fmt_num(m->dens) + " per km\xc2\xb2";
    if (m->med > 0) s += " \xc2\xb7 median value R" + fmt_num(m->med);
    L.emplace_back("People and property", s);
    if (!m->win.empty())
      L.emplace_back("Elections", m->win + " won " + std::to_string(m->win_wards) + " of " + std::to_string(m->wards) +
                                      " wards in " + m->name);
  }
  auto near = [&](const std::vector<Site>& v, bool (*ok)(const Site&)) -> std::pair<const Site*, double> {
    const Site* best = nullptr;
    double bd = 1e9;
    for (const auto& o : v) {
      if (ok && !ok(o)) continue;
      const double dd = km_between(lon, lat, o.lon, o.lat);
      if (dd < bd) bd = dd, best = &o;
    }
    return {best, bd};
  };
  if (auto [o, km] = near(d.places, nullptr); o) L.emplace_back("Nearest town", o->name + " \xc2\xb7 " + fmt_km(km));
  if (auto [o, km] = near(d.dams, nullptr); o)
    L.emplace_back("Nearest dam", o->name + " \xc2\xb7 " + fmt_num(o->a) + "% full \xc2\xb7 " + fmt_km(km));
  if (auto [o, km] = near(d.cap, [](const Site& s) { return s.a > 0; }); o && km <= 150)
    L.emplace_back("Grid capacity", o->name + " has " + fmt_num(o->a) + " MW free \xc2\xb7 " + fmt_km(km));
  else if (!d.cap.empty())
    L.emplace_back("Grid capacity", "No substation with free capacity within 150 km (NTCSA 2025)");
  if (auto [o, km] = near(d.air, nullptr); o)
    L.emplace_back("Nearest airport", o->name + (o->t.empty() ? "" : " (" + o->t + ")") + " \xc2\xb7 " + fmt_km(km));
  if (auto [o, km] = near(d.ports, nullptr); o && km < 300) L.emplace_back("Nearest port", o->name + " \xc2\xb7 " + fmt_km(km));
  if (!rules.empty() && m) {
    std::vector<std::string> fails;
    for (const auto& r : rules)
      if (!pass(d, *m, r)) {
        std::string l = def(r.k)->label;
        for (const char* tail : {", at least", ", at most"})
          if (l.size() > std::strlen(tail) && l.compare(l.size() - std::strlen(tail), std::string::npos, tail) == 0)
            l.erase(l.size() - std::strlen(tail));
        fails.push_back(l);
      }
    std::string s;
    if (fails.empty()) s = "Meets all " + std::to_string(rules.size());
    else {
      s = "Fails " + std::to_string(fails.size()) + " of " + std::to_string(rules.size()) + ": ";
      for (size_t i = 0; i < fails.size(); ++i) s += (i ? "; " : "") + fails[i];
    }
    L.emplace_back("Your Find conditions", s);
  }
  return L;
}

}  // namespace gsa::find
