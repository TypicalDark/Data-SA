#include "map_labels.hpp"

#include <algorithm>
#include <cmath>

namespace gsa::map_labels {

using style::Topic;

float fade(double zoom, double minz) { return float(std::clamp((zoom - (minz - 0.7)) / 0.7, 0.0, 1.0)); }

namespace {

constexpr int kProvince = 0, kFacility = 10, kMunicipality = 40, kLine = 60;

bool owns(const mvt::Point& p) { return p.x >= 0 && p.y >= 0 && p.x < 1 && p.y < 1; }

std::string fmt_kv(double kv) { return std::to_string(int(std::lround(kv))) + " kV"; }

void admin(const TileOnScreen& t, double zoom, const Fonts& fonts, text::Engine& engine) {
  const auto* l = t.tile->layer("label");
  if (!l) return;
  const uint32_t halo = style::hex("#0b0f13", 0.92f).packed();
  for (const auto& f : l->features) {
    if (f.type != mvt::GeomType::Point || f.parts.empty() || f.parts[0].empty()) continue;
    const auto& p = f.parts[0][0];
    if (!owns(p)) continue;  // the neighbouring tile owns it
    const int level = int(f.num("level").value_or(3));
    const std::string name = f.str("name");
    if (name.empty()) continue;
    text::PointLabel c;
    c.key = "a" + std::to_string(level) + ":" + name;
    c.text = name;
    c.x = float(t.ox + p.x * t.k);
    c.y = float(t.oy + p.y * t.k);
    if (level <= 1) {
      if (zoom > 8.5) continue;  // provinces give way to detail
      c.style = {fonts.medium, 15, style::hex("#e6edf3").packed(), halo, 1.6f, 0.02f};
      c.priority = kProvince;
      c.opacity = fade(zoom, 3.0);
    } else {
      c.style = {fonts.regular, 12.5f, style::hex("#b8c2cc").packed(), halo, 1.5f, 0};
      c.priority = kMunicipality;
      c.opacity = fade(zoom, level <= 3 ? 6.6 : 11.0);
    }
    if (c.opacity > 0) engine.add(std::move(c));
  }
}

void grid(const TileOnScreen& t, const layers::MapStyle& ms, double zoom, const Fonts& fonts, text::Engine& engine) {
  const uint32_t bg = style::background().packed();
  const uint32_t halo = style::hex("#0b0f13", 0.92f).packed();
  if (const auto* l = t.tile->layer("facility"); l && (ms.subs || ms.plants)) {
    for (size_t fi = 0; fi < l->features.size(); ++fi) {
      const auto& f = l->features[fi];
      if (f.type != mvt::GeomType::Point || f.parts.empty() || f.parts[0].empty()) continue;
      const auto& p = f.parts[0][0];
      if (!owns(p)) continue;
      const bool station = f.str("kind") == "power_station";
      if (station ? !ms.plants : !ms.subs) continue;
      const double kv = f.num("kv").value_or(0), mw = f.num("mw").value_or(0);
      const double minz = station ? (mw >= 500 ? 4.0 : mw >= 50 ? 6.0 : 7.5) : (kv >= 275 ? 5.5 : kv >= 132 ? 7.0 : 8.0);
      const float opacity = fade(zoom, minz);
      if (opacity <= 0) continue;
      text::PointLabel c;
      c.key = "f:" + f.str("id");
      c.x = float(t.ox + p.x * t.k);
      c.y = float(t.oy + p.y * t.k);
      c.opacity = opacity;
      text::IconSpec ic;
      ic.kind = station ? text::Icon::Circle : text::Icon::Square;
      ic.radius = station ? float(std::clamp(2.5 + std::sqrt(mw) / 5.0, 3.0, 11.0)) : (kv >= 400 ? 4.6f : kv >= 220 ? 4.0f : 3.2f);
      ic.fill = (station ? style::plant(f.str("fuel")) : style::volt(kv)).packed();
      ic.halo = bg;
      ic.halo_px = 1.4f;
      c.icon = ic;
      // names a little after the symbol, the biggest first
      if (zoom >= minz + (station ? 1.0 : 1.6)) {
        c.text = f.str("name");
        if (station && mw > 0) c.text += "  " + std::to_string(int(std::lround(mw))) + " MW";
      }
      c.style = {fonts.regular, 11.5f, style::hex("#aab6c2").packed(), halo, 1.5f, 0};
      if (t.picks) {
        c.pick = int(t.picks->size());
        const double n = double(1u << t.id.z);
        t.picks->push_back({t.tile, "facility", fi, (double(t.id.x) + double(p.x)) / n, (double(t.id.y) + double(p.y)) / n});
      }
      c.priority = kFacility + (station ? int(std::max(0.0, 20 - std::sqrt(mw) / 3)) : int(std::max(0.0, 25 - kv / 20)));
      engine.add(std::move(c));
    }
  }
  if (const auto* l = t.tile->layer("line"); l && ms.lines) {
    const text::Rect owner{float(t.ox), float(t.oy), float(t.ox + t.k), float(t.oy + t.k)};
    int fi = 0;
    for (const auto& f : l->features) {
      ++fi;
      if (f.type != mvt::GeomType::Line) continue;
      const double kv = f.num("kv").value_or(0);
      if (kv < 132 || f.num("inferred").value_or(0) > 0) continue;
      const double minz = kv >= 400 ? 7.0 : kv >= 220 ? 8.0 : 9.0;
      const float opacity = fade(zoom, minz);
      if (opacity <= 0) continue;
      int pi = 0;
      for (const auto& part : f.parts) {
        ++pi;
        if (part.size() < 2) continue;
        text::LineLabel c;
        c.key = "l:" + std::to_string(t.id.z) + "/" + std::to_string(t.id.x) + "/" + std::to_string(t.id.y) + ":" +
                std::to_string(fi) + "." + std::to_string(pi);
        c.text = fmt_kv(kv);
        c.pts.reserve(part.size());
        for (const auto& q : part) c.pts.push_back({float(t.ox + q.x * t.k), float(t.oy + q.y * t.k)});
        c.owner = owner;
        // light ink reads on every line colour; the line itself carries the voltage colour
        c.style = {fonts.mono, 10.5f, style::hex("#d5dde5").packed(), halo, 1.8f, 0.02f};
        c.priority = kLine + int(std::max(0.0, 8 - kv / 60));
        c.opacity = opacity;
        engine.add(std::move(c));
      }
    }
  }
}

}  // namespace

const mvt::Feature& FeatureRef::feature() const { return tile->layer(layer)->features.at(index); }

void collect(const TileOnScreen& t, const layers::MapStyle& ms, double zoom, const Fonts& fonts, text::Engine& engine) {
  if (!t.tile) return;
  if (t.pack == "admin" && ms.names) admin(t, zoom, fonts, engine);
  if (t.pack == "grid") grid(t, ms, zoom, fonts, engine);
}

}  // namespace gsa::map_labels
