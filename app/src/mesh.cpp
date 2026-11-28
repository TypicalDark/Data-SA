#include "mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <mapbox/earcut.hpp>

namespace gsa::mesh {

using style::Topic;

void add_polygon(const mvt::Feature& f, uint32_t rgba, std::vector<Vertex>& out) {
  for (const auto& rings : mvt::group_polygons(f)) {
    std::vector<std::vector<std::array<float, 2>>> poly;
    std::vector<const mvt::Point*> flat;
    for (size_t ri : rings) {
      auto& ring = poly.emplace_back();
      const auto& src = f.parts[ri];
      // earcut wants open rings
      const size_t n = (src.size() > 1 && src.front().x == src.back().x && src.front().y == src.back().y) ? src.size() - 1
                                                                                                        : src.size();
      for (size_t i = 0; i < n; ++i) {
        ring.push_back({src[i].x, src[i].y});
        flat.push_back(&src[i]);
      }
    }
    const auto idx = mapbox::earcut<uint32_t>(poly);
    for (uint32_t i : idx) out.push_back({flat[i]->x, flat[i]->y, 0, 0, rgba, 0});
  }
}

void add_line(const mvt::Ring& r, float hw, uint32_t rgba, float minz, bool dashed, std::vector<Vertex>& out) {
  for (size_t i = 0; i + 1 < r.size(); ++i) {
    if (dashed && (i % 2)) continue;  // coarse dash: every other segment
    const float x0 = r[i].x, y0 = r[i].y, x1 = r[i + 1].x, y1 = r[i + 1].y;
    const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0) continue;
    // the map has no rotation, so a tile-space normal is also the screen-space normal
    const float nx = -dy / len * hw, ny = dx / len * hw;
    // extend each segment by half a width so joins have no gaps
    const float ex = dx / len * hw * 0.5f, ey = dy / len * hw * 0.5f;
    const Vertex a{x0, y0, nx - ex, ny - ey, rgba, minz}, b{x0, y0, -nx - ex, -ny - ey, rgba, minz};
    const Vertex c{x1, y1, nx + ex, ny + ey, rgba, minz}, d{x1, y1, -nx + ex, -ny + ey, rgba, minz};
    out.insert(out.end(), {a, b, c, b, d, c});
  }
}

void add_square(float x, float y, float h, uint32_t rgba, float minz, std::vector<Vertex>& out) {
  const Vertex a{x, y, -h, -h, rgba, minz}, b{x, y, h, -h, rgba, minz}, c{x, y, h, h, rgba, minz}, d{x, y, -h, h, rgba, minz};
  out.insert(out.end(), {a, b, c, a, c, d});
}

namespace {

void outline(const mvt::Feature& f, float hw, uint32_t rgba, float minz, std::vector<Vertex>& out) {
  for (const auto& ring : f.parts) add_line(ring, hw, rgba, minz, false, out);
}

float kv_minz(double kv) { return kv >= 400 ? 4.0f : kv >= 220 ? 5.0f : kv >= 132 ? 6.0f : 7.0f; }

// 3D buildings: walls shaded by the way they face, then the roof, the farthest (northern) buildings
// first so nearer ones cover them (there is no depth buffer).
void extrude_buildings(const mvt::Layer& l, std::vector<Vertex>& out) {
  std::vector<std::pair<float, size_t>> order;
  order.reserve(l.features.size());
  for (size_t i = 0; i < l.features.size(); ++i) {
    const auto& f = l.features[i];
    if (f.parts.empty() || f.parts[0].size() < 3) continue;
    float y = 0;
    for (const auto& p : f.parts[0]) y = std::max(y, p.y);
    order.push_back({y, i});
  }
  std::sort(order.begin(), order.end());
  const style::Rgba roof = style::hex("#2e4050"), dark = style::hex("#151f27"), lit = style::hex("#3d5366");
  // light from the north-west, as the hillshade
  const float lx = -0.7071f, ly = -0.7071f;
  for (const auto& [y, i] : order) {
    const auto& f = l.features[i];
    const float h = float(f.num("h").value_or(4));
    const auto& r = f.parts[0];
    const size_t n = (r.size() > 3 && r.front().x == r.back().x && r.front().y == r.back().y) ? r.size() - 1 : r.size();
    double area = 0;
    for (size_t j = 0; j < n; ++j) area += double(r[j].x) * r[(j + 1) % n].y - double(r[(j + 1) % n].x) * r[j].y;
    const float sgn = area > 0 ? 1.0f : -1.0f;  // outward normal side for either winding (y down)
    for (size_t j = 0; j < n; ++j) {
      const auto& a = r[j];
      const auto& b = r[(j + 1) % n];
      const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
      if (len <= 0) continue;
      const float nx = sgn * dy / len, ny = -sgn * dx / len;
      if (ny < -0.05f) continue;  // walls facing north are behind the building from this view
      const float t = std::clamp(0.5f + 0.5f * (nx * lx + ny * ly), 0.0f, 1.0f);
      style::Rgba c;
      c.r = uint8_t(dark.r + (lit.r - dark.r) * t);
      c.g = uint8_t(dark.g + (lit.g - dark.g) * t);
      c.b = uint8_t(dark.b + (lit.b - dark.b) * t);
      c.a = 245;
      const uint32_t cp = c.packed();
      const Vertex a0{a.x, a.y, 0, 0, cp, 14.3f, 0}, b0{b.x, b.y, 0, 0, cp, 14.3f, 0};
      const Vertex a1{a.x, a.y, 0, 0, cp, 14.3f, h}, b1{b.x, b.y, 0, 0, cp, 14.3f, h};
      out.insert(out.end(), {a0, b0, b1, a0, b1, a1});
    }
    const size_t before = out.size();
    add_polygon(f, style::Rgba{roof.r, roof.g, roof.b, 250}.packed(), out);
    for (size_t k = before; k < out.size(); ++k) {
      out[k].minz = 14.3f;
      out[k].h = h;
    }
  }
}

// The website's base map (dark theme): land use under everything, then roads with casings and rail.
// Tiles below zoom 10 are the overview (main roads only), from 10 the regional tiles (every class).
void base_map(const mvt::Tile& tile, bool overview, Mesh& out) {
  if (const auto* l = tile.layer("land")) {
    for (const auto& f : l->features) {
      if (f.type != mvt::GeomType::Polygon) continue;
      const auto c = style::land_use(f.str("c"), overview);
      if (c.a) add_polygon(f, c.packed(), out.fill);
    }
  }
  if (const auto* l = tile.layer("rail")) {
    const auto c = style::hex("#55626c", overview ? 0.67f : 1.0f).packed();
    for (const auto& f : l->features)
      if (f.type == mvt::GeomType::Line)
        for (const auto& part : f.parts) add_line(part, overview ? 0.5f : 1.0f, c, 6.5f, false, out.lines);
  }
  const auto* l = tile.layer("road");
  if (!l) return;
  // casings first, the biggest roads last so they sit on top
  for (int pass = 0; pass < 2; ++pass) {
    for (int r = 7; r >= 0; --r) {
      const auto road = style::road(r, pass == 0);
      const float hw = style::road_half_width(r, overview) + (pass == 0 ? (overview ? 0.6f : 0.9f) : 0.0f);
      const float minz = style::road_minz(r, overview);
      auto c = road;
      if (overview && pass == 1 && r >= 2) c.a = uint8_t(c.a * (r == 2 ? 0.8f : 0.75f));
      for (const auto& f : l->features) {
        if (f.type != mvt::GeomType::Line || int(f.num("r").value_or(7)) != r) continue;
        for (const auto& part : f.parts) add_line(part, hw, c.packed(), minz, false, out.lines);
      }
    }
  }
}

}  // namespace

void build(const std::string& pack, const mvt::Tile& tile, const layers::MapStyle& ms, Mesh& out, int z) {
  if (pack == "admin") {
    const uint32_t nodata = (ms.fill == style::Topic::Overview ? style::land() : style::hex("#16222b")).packed();
    if (const auto* l = tile.layer("municipality")) {
      for (const auto& f : l->features) {
        if (f.type != mvt::GeomType::Polygon) continue;
        const auto c = style::muni_fill(ms.fill, f);
        add_polygon(f, c ? c->packed() : nodata, out.fill);
        outline(f, 0.35f, style::muni_border().packed(), 5.2f, out.lines);
        if (ms.find_on) {
          // with the lines so the shade covers every pack's fills and images (the website's findpass layer)
          if (ms.find_pass.count(f.str("id"))) {
            auto a = style::accent();
            a.a = 230;
            outline(f, 1.0f, a.packed(), 0, out.lines);
          } else {
            add_polygon(f, style::Rgba{6, 10, 14, 150}.packed(), out.lines);
          }
        }
      }
    }
    if (const auto* l = tile.layer("province")) {
      for (const auto& f : l->features)
        if (f.type == mvt::GeomType::Polygon) outline(f, 0.7f, style::prov_border().packed(), 0, out.lines);
    }
    return;
  }
  if (pack == "buildings") {
    // footprints over the roads, as on the website (from zoom 13.3, fully shown at 14.3)
    if (!ms.buildings) return;
    if (const auto* l = tile.layer("building")) {
      if (ms.extrude) {
        extrude_buildings(*l, out.lines);
        return;
      }
      const uint32_t c = style::hex("#22303b", 0.88f).packed();
      for (const auto& f : l->features) {
        if (f.parts.empty() || f.parts[0].size() < 3) continue;
        const size_t before = out.lines.size();
        add_polygon(f, c, out.lines);
        for (size_t i = before; i < out.lines.size(); ++i) out.lines[i].minz = 14.3f;
      }
    }
    return;
  }
  if (pack == "base") {
    if (ms.roads) base_map(tile, z < 10, out);
    return;
  }
  if (pack.rfind("web-", 0) == 0 || layers::is_plot_pack(pack)) {
    // website layers: areas tinted with an outline, lines, and points as small squares
    for (const auto& e : ms.extra) {
      const auto* l = tile.layer(e.layer);
      if (!l) continue;
      style::Rgba base;
      base.r = uint8_t(e.rgba & 0xff);
      base.g = uint8_t(e.rgba >> 8 & 0xff);
      base.b = uint8_t(e.rgba >> 16 & 0xff);
      base.a = 255;
      auto with_alpha = [](style::Rgba c, float a) {
        c.a = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255);
        return c.packed();
      };
      for (const auto& f : l->features) {
        switch (f.type) {
          case mvt::GeomType::Polygon: {
            if (e.mode == layers::Extra::Outline) {
              outline(f, 0.45f, with_alpha(base, 0.8f), 0, out.lines);
            } else if (e.mode == layers::Extra::Plots) {
              // the website's plots: value bands, outlines once the plots are big enough to read
              if (const auto v = f.num("value"); v && *v > 0) add_polygon(f, style::value_band(*v, 0.55f).packed(), out.fill);
              outline(f, 0.35f, style::hex("#c9d6de", 0.35f).packed(), 13.5f, out.lines);
            } else if (e.mode == layers::Extra::Party) {
              const bool pass = !ms.filter.vote_on() ||
                                layers::pass_ward(ms.filter, f.str("party"), f.num("turnout").value_or(-1), f.str("muni"));
              if (!pass && ms.filter.hide) break;
              add_polygon(f, style::party(f.str("party"), pass ? 0.8f : 0.1f).packed(), out.fill);
              outline(f, 0.3f, style::hex("#0b161d", 0.6f).packed(), 8.0f, out.lines);
            } else {
              // layers that tile the whole country (service areas, who supplies) stay a wash so the grid reads on top
              const bool cover = e.layer == "zones" || e.layer == "x_supauth";
              add_polygon(f, with_alpha(base, cover ? 0.07f : 0.26f), out.fill);
              outline(f, cover ? 0.4f : 0.5f, with_alpha(base, cover ? 0.5f : 0.85f), cover ? 8.0f : 7.0f, out.lines);  // edges once zoomed in
            }
            break;
          }
          case mvt::GeomType::Line: {
            // flight routes are many long arcs: keep them faint so the country stays readable
            const bool faint = e.layer == "x_flights";
            float lw = faint ? 0.5f : 0.8f, la = faint ? 0.35f : 0.9f;
            if (e.layer == "rivers") {  // the website's widthScale: thin at country zooms, full width from z12
              lw = std::clamp(0.25f + float(z - 5) * 0.1f, 0.25f, 0.95f) * (f.num("k").value_or(0) == 1 ? 0.7f : 1.0f);
              la = z < 8 ? 0.7f : 0.85f;
            }
            for (const auto& part : f.parts) add_line(part, lw, with_alpha(base, la), 0, false, out.lines);
            break;
          }
          case mvt::GeomType::Point:
            for (const auto& part : f.parts)
              for (const auto& p : part) {
                if (p.x < 0 || p.y < 0 || p.x >= 1 || p.y >= 1) continue;
                add_square(p.x, p.y, 3.2f, style::hex("#070d12", 0.9f).packed(), 0, out.symbols);
                add_square(p.x, p.y, 2.2f, base.packed(), 0, out.symbols);
              }
            break;
          default: break;
        }
      }
    }
    return;
  }
  if (pack == "grid") {
    if (!ms.lines) return;  // facilities draw as icons in the text pass
    if (const auto* l = tile.layer("line")) {
      for (const auto& f : l->features) {
        if (f.type != mvt::GeomType::Line) continue;
        const double kv = f.num("kv").value_or(0);
        const bool inferred = f.num("inferred").value_or(0) > 0;
        const bool pass = layers::pass_volt(ms.filter, kv);
        if (!pass && ms.filter.hide) continue;
        auto c = inferred ? style::hex("#7d8b97", 0.9f) : style::volt(kv);
        c.a = uint8_t(c.a * (pass ? 0.92f : 0.12f));
        for (const auto& part : f.parts) add_line(part, style::line_width_px(kv, true) * 0.5f, c.packed(), kv_minz(kv), inferred, out.lines);
      }
    }
    // facilities are icons in the text pass (map_labels.cpp)
  }
}

void labels(const mvt::Tile& tile, double ox, double oy, double size, std::vector<Label>& out) {
  const auto* l = tile.layer("label");
  if (!l) return;
  for (const auto& f : l->features) {
    if (f.type != mvt::GeomType::Point || f.parts.empty() || f.parts[0].empty()) continue;
    const auto& p = f.parts[0][0];
    if (p.x < 0 || p.y < 0 || p.x >= 1 || p.y >= 1) continue;  // the neighbour tile owns it
    const int level = int(f.num("level").value_or(3));
    out.push_back({f.str("name"), ox + p.x * size, oy + p.y * size, level, level <= 1 ? 3.0f : level <= 3 ? 6.6f : 11.0f});
  }
}

}  // namespace gsa::mesh
