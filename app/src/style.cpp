#include "style.hpp"

#include <cstdlib>
#include <map>

namespace gsa::style {

Rgba hex(const char* h, float alpha) {
  const unsigned long n = std::strtoul(h + (h[0] == '#'), nullptr, 16);
  return Rgba{uint8_t(n >> 16), uint8_t(n >> 8 & 255), uint8_t(n & 255), uint8_t(alpha * 255.0f + 0.5f)};
}

const std::array<TopicInfo, size_t(Topic::Count)>& topics() {
  static const std::array<TopicInfo, size_t(Topic::Count)> t{{
      {Topic::Overview, "Overview", "1", "#8794a1",
       "Towns and municipalities with nothing coloured. Pick a topic or search for a place.", ""},
      {Topic::People, "People", "2", "#c98bd0", "People per km2 by municipality, Census 2022.", ""},
      {Topic::Property, "Property", "3", "#de4968", "Median market value per municipality from the valuation rolls.", ""},
      {Topic::Water, "Water", "4", "#6baed6", "Dam levels from the DWS weekly report.", "water"},
      {Topic::Energy, "Energy and grid", "5", "#e0a030", "Power lines by voltage, substations and power stations.", ""},
      {Topic::Elections, "Elections", "6", "#2e9e4f", "2026 ward winners, summarised per municipality.", ""},
      {Topic::Transport, "Transport", "7", "#e6edf3", "Airports, ports, and live planes and ships.", "transport"},
  }};
  return t;
}
const TopicInfo& info(Topic t) { return topics()[size_t(t)]; }

namespace {
const char* kDens[] = {"#2b1a3a", "#5c1f5b", "#a3195b", "#e0463e", "#f8a77a"};
const double kDensBrk[] = {10, 30, 80, 300};
const char* kValue[] = {"#3b0f70", "#8c2981", "#de4968", "#fe9f6d", "#fcfdbf"};
const double kValueBrk[] = {200e3, 400e3, 700e3, 1.2e6};
const std::map<std::string, const char*> kParty = {{"ANC", "#2e9e4f"}, {"DA", "#2f78d6"},      {"MK", "#c9a227"},
                                                   {"IFP", "#d9483b"}, {"EFF", "#9b2a52"},     {"PA", "#9d6ad6"},
                                                   {"ActionSA", "#38b2a3"}, {"Other", "#7d8b97"}};
template <size_t N>
Rgba ramp(const char* (&cols)[N], const double (&brk)[N - 1], double v, float a) {
  size_t i = 0;
  while (i < N - 1 && v >= brk[i]) ++i;
  return hex(cols[i], a);
}
}  // namespace

std::optional<Rgba> muni_fill(Topic t, const mvt::Feature& f) {
  constexpr float a = 0.78f;
  switch (t) {
    case Topic::People:
      if (auto d = f.num("density")) return ramp(kDens, kDensBrk, *d, a);
      return std::nullopt;
    case Topic::Property: {
      auto v = f.num("value_median");
      if (!v) return std::nullopt;
      const auto n = f.num("parcels").value_or(0);
      return ramp(kValue, kValueBrk, *v, n < 500 ? a * 0.55f : a);  // paler under 500 properties
    }
    case Topic::Elections: {
      const auto w = f.str("win");
      if (w.empty()) return std::nullopt;
      auto it = kParty.find(w);
      return hex(it != kParty.end() ? it->second : kParty.at("Other"), a);
    }
    default: return land();
  }
}

Rgba land() { return hex("#121820"); }
Rgba background() { return hex("#0b0f13"); }
Rgba muni_border() { return hex("#36414d", 0.8f); }
Rgba prov_border() { return hex("#6b7886", 0.9f); }
Rgba accent() { return hex("#35d0f0"); }

Rgba volt(double kv) {
  if (kv >= 400) return hex("#b0207e");
  if (kv >= 220) return hex("#e0302d");
  if (kv >= 100) return hex("#ef7f1a");
  if (kv >= 50) return hex("#c9a20a");
  if (kv >= 1) return hex("#2aa36b");
  return hex("#7d8b97");
}

Rgba plant(const std::string& fuel) {
  static const std::map<std::string, const char*> c = {
      {"coal", "#3e3e3e"},  {"gas", "#d86f00"},     {"oil", "#8d5524"},   {"diesel", "#8d5524"}, {"nuclear", "#7b1fa2"},
      {"hydro", "#1e78d2"}, {"solar", "#e2a400"},   {"wind", "#1f968a"},  {"biomass", "#5d8a1f"}, {"battery", "#c2185b"}};
  auto it = c.find(fuel);
  return hex(it != c.end() ? it->second : "#78909c");
}

float line_width_px(double kv, bool focus) {
  const int w = kv >= 400 ? 3 : kv >= 220 ? 2 : kv >= 132 ? 1 : 0;
  static const float f[] = {0.9f, 1.2f, 1.7f, 2.4f}, g[] = {0.6f, 0.8f, 1.1f, 1.5f};
  return focus ? f[w] : g[w];
}

bool line_visible(double kv, double zoom) {
  if (kv >= 400) return zoom >= 3.3;
  if (kv >= 220) return zoom >= 4.3;
  if (kv >= 132) return zoom >= 5.3;
  return zoom >= 6.3;
}

Legend legend(Topic t) {
  Legend l;
  switch (t) {
    case Topic::People:
      l.title = "People per km2";
      for (int i = 0; i < 5; ++i) {
        static const char* lab[] = {"under 10", "10 to 30", "30 to 80", "80 to 300", "300 and up"};
        l.items.push_back({lab[i], hex(kDens[i])});
      }
      l.note = "Stats SA Census 2022";
      break;
    case Topic::Property:
      l.title = "Median property value";
      for (int i = 0; i < 5; ++i) {
        static const char* lab[] = {"under R200k", "R200k to R400k", "R400k to R700k", "R700k to R1.2m", "R1.2m and up"};
        l.items.push_back({lab[i], hex(kValue[i])});
      }
      l.note = "Municipal valuation rolls; paler = under 500 properties";
      break;
    case Topic::Elections:
      l.title = "2026 ward winners";
      for (const auto& [p, c] : kParty) l.items.push_back({p, hex(c)});
      l.note = "IEC LGE 2026, most wards per municipality";
      break;
    case Topic::Energy:
      l.title = "Power lines";
      l.items = {{"400 kV and up", volt(400)}, {"220 to 399 kV", volt(220)}, {"100 to 219 kV", volt(132)},
                 {"50 to 99 kV", volt(66)}};
      l.note = "OpenStreetMap; dashed = inferred joins";
      break;
    default: break;
  }
  return l;
}

}  // namespace gsa::style
