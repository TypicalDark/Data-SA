#include "labels.hpp"

#include <algorithm>
#include <cmath>

namespace gsa::text {

uint32_t rgba(uint32_t packed, float a) {
  const float alpha = float(packed >> 24) * std::clamp(a, 0.0f, 1.0f);
  return (packed & 0x00ffffffu) | (uint32_t(std::lround(alpha)) << 24);
}

// ------------------------------------------------------------------ collision

void CollisionIndex::reset(float w, float h, float cell) {
  cell_ = cell;
  cols_ = std::max(1, int(std::ceil(w / cell)));
  rows_ = std::max(1, int(std::ceil(h / cell)));
  boxes_.clear();
  grid_.assign(size_t(cols_) * size_t(rows_), {});
}

void CollisionIndex::cells(const Rect& r, int& cx0, int& cy0, int& cx1, int& cy1) const {
  cx0 = std::clamp(int(std::floor(r.x0 / cell_)), 0, cols_ - 1);
  cy0 = std::clamp(int(std::floor(r.y0 / cell_)), 0, rows_ - 1);
  cx1 = std::clamp(int(std::floor(r.x1 / cell_)), 0, cols_ - 1);
  cy1 = std::clamp(int(std::floor(r.y1 / cell_)), 0, rows_ - 1);
}

bool CollisionIndex::free(const Rect& r) const {
  if (grid_.empty()) return true;
  int cx0, cy0, cx1, cy1;
  cells(r, cx0, cy0, cx1, cy1);
  for (int cy = cy0; cy <= cy1; ++cy)
    for (int cx = cx0; cx <= cx1; ++cx)
      for (uint32_t i : grid_[size_t(cy) * size_t(cols_) + size_t(cx)])
        if (boxes_[i].overlaps(r)) return false;
  return true;
}

void CollisionIndex::insert(const Rect& r) {
  if (grid_.empty()) return;
  const auto id = uint32_t(boxes_.size());
  boxes_.push_back(r);
  int cx0, cy0, cx1, cy1;
  cells(r, cx0, cy0, cx1, cy1);
  for (int cy = cy0; cy <= cy1; ++cy)
    for (int cx = cx0; cx <= cx1; ++cx) grid_[size_t(cy) * size_t(cols_) + size_t(cx)].push_back(id);
}

// ------------------------------------------------------------------ shaping

Run shape(Atlas& atlas, const Style& st, const std::string& text) {
  Run run;
  const float s = st.size / float(Atlas::kBase);
  const auto cps = utf8_decode(text);
  float pen = 0;
  for (size_t i = 0; i < cps.size(); ++i) {
    if (i) pen += atlas.kern(st.font, cps[i - 1], cps[i]) * s + st.spacing_em * st.size;
    const Glyph& g = atlas.glyph(st.font, cps[i]);
    run.glyphs.push_back({g, pen, g.advance * s});
    pen += g.advance * s;
  }
  run.width = pen;
  run.ascent = atlas.ascent(st.font) * s;
  run.descent = atlas.descent(st.font) * s;
  return run;
}

namespace {

constexpr float kFadeSeconds = 0.2f;
constexpr float kPi = 3.14159265358979f;

float halo_units(float halo_px, float scale) {
  // pixels on screen -> distance-field texels -> 0..1 of the texel range
  return halo_px / scale * Atlas::kDistScale / 255.0f;
}

void quad(std::vector<Vertex>& out, const std::array<float, 2> c[4], const Glyph& g, uint32_t fill, uint32_t halo,
          float halo_w) {
  const Vertex v0{c[0][0], c[0][1], g.u0, g.v0, fill, halo, halo_w, 1};
  const Vertex v1{c[1][0], c[1][1], g.u1, g.v0, fill, halo, halo_w, 1};
  const Vertex v2{c[2][0], c[2][1], g.u1, g.v1, fill, halo, halo_w, 1};
  const Vertex v3{c[3][0], c[3][1], g.u0, g.v1, fill, halo, halo_w, 1};
  out.insert(out.end(), {v0, v1, v2, v0, v2, v3});
}

Rect text_box(float tx, float baseline, const Run& run, float size) {
  return {tx - 2, baseline - size * 0.82f, tx + run.width + 2, baseline + size * 0.28f};
}

}  // namespace

// ------------------------------------------------------------------ engine

void Engine::begin(float w, float h, const std::vector<Rect>& blockers) {
  w_ = w;
  h_ = h;
  view_ = {0, 0, w, h};
  blockers_ = blockers;
  points_.clear();
  lines_.clear();
  line_anchors_.clear();
  hits_.clear();
  index_.reset(w, h);
  ++frame_;
}

float Engine::step(const std::string& key, bool placed, double dt) {
  auto& f = fades_[key];
  f.seen = frame_;
  const float target = placed ? 1.0f : 0.0f;
  if (dt < 0) {
    f.a = target;
  } else {
    const float d = float(dt) / kFadeSeconds;
    f.a = placed ? std::min(1.0f, f.a + d) : std::max(0.0f, f.a - d);
  }
  if (f.a != target) animating_ = true;
  return f.a;
}

void Engine::emit_point(const PointLabel& l, const Run& run, float tx, float ty, float a, bool with_text, bool with_icon,
                        std::vector<Vertex>& out) {
  if (with_icon && l.icon) {
    const auto& ic = *l.icon;
    const Glyph& g = atlas_.icon(ic.kind);
    const float s = ic.radius / float(Atlas::kIconRadius);
    const std::array<float, 2> c[4] = {{l.x + g.x0 * s, l.y + g.y0 * s},
                                       {l.x + g.x1 * s, l.y + g.y0 * s},
                                       {l.x + g.x1 * s, l.y + g.y1 * s},
                                       {l.x + g.x0 * s, l.y + g.y1 * s}};
    quad(out, c, g, rgba(ic.fill, l.opacity), rgba(ic.halo, l.opacity), halo_units(ic.halo_px, s));
  }
  if (!with_text || a <= 0.003f) return;
  const float s = l.style.size / float(Atlas::kBase);
  const uint32_t fill = rgba(l.style.fill, a * l.opacity), halo = rgba(l.style.halo, a * l.opacity);
  const float hw = halo_units(l.style.halo_px, s);
  for (const auto& sg : run.glyphs) {
    if (sg.g.x1 <= sg.g.x0) continue;
    const float x = tx + sg.x;
    const std::array<float, 2> c[4] = {{x + sg.g.x0 * s, ty + sg.g.y0 * s},
                                       {x + sg.g.x1 * s, ty + sg.g.y0 * s},
                                       {x + sg.g.x1 * s, ty + sg.g.y1 * s},
                                       {x + sg.g.x0 * s, ty + sg.g.y1 * s}};
    quad(out, c, sg.g, fill, halo, hw);
  }
}

bool Engine::place_line(const LineLabel& l, const Run& run, size_t, float d, const std::vector<float>& cum,
                        std::vector<Vertex>* out, float a, std::vector<Rect>& boxes) {
  // Walks glyph centres along the line; fails on sharp bends or when the text runs off the end.
  const auto& p = l.pts;
  const float total = cum.back();
  const float start = d - run.width / 2;
  if (start < 0 || start + run.width > total) return false;
  const float s = l.style.size / float(Atlas::kBase);
  const float base_off = l.style.size * 0.34f;  // baseline below the line so the text sits centred on it
  const uint32_t fill = rgba(l.style.fill, a * l.opacity), halo = rgba(l.style.halo, a * l.opacity);
  const float hw = halo_units(l.style.halo_px, s);
  const float max_bend = l.max_bend_deg * kPi / 180;
  size_t seg = 0;
  float prev_angle = 0;
  bool first = true;
  boxes.clear();
  std::vector<Vertex> tmp;
  for (const auto& sg : run.glyphs) {
    const float c = start + sg.x + sg.adv / 2;
    while (seg + 2 < cum.size() && cum[seg + 1] < c) ++seg;
    const float sl = cum[seg + 1] - cum[seg];
    const float t = sl > 0 ? (c - cum[seg]) / sl : 0;
    const float px = p[seg][0] + (p[seg + 1][0] - p[seg][0]) * t, py = p[seg][1] + (p[seg + 1][1] - p[seg][1]) * t;
    const float angle = std::atan2(p[seg + 1][1] - p[seg][1], p[seg + 1][0] - p[seg][0]);
    if (!first) {
      float turn = std::abs(angle - prev_angle);
      if (turn > kPi) turn = 2 * kPi - turn;
      if (turn > max_bend) return false;
    }
    first = false;
    prev_angle = angle;
    const float half = l.style.size * 0.55f;
    boxes.push_back({px - half, py - half, px + half, py + half});
    if (!out || sg.g.x1 <= sg.g.x0) continue;
    const float co = std::cos(angle), si = std::sin(angle);
    const float qx0 = sg.g.x0 * s - sg.adv / 2, qx1 = sg.g.x1 * s - sg.adv / 2;
    const float qy0 = sg.g.y0 * s + base_off, qy1 = sg.g.y1 * s + base_off;
    auto rot = [&](float qx, float qy) { return std::array<float, 2>{px + qx * co - qy * si, py + qx * si + qy * co}; };
    const std::array<float, 2> q[4] = {rot(qx0, qy0), rot(qx1, qy0), rot(qx1, qy1), rot(qx0, qy1)};
    quad(tmp, q, sg.g, fill, halo, hw);
  }
  if (out) out->insert(out->end(), tmp.begin(), tmp.end());
  return true;
}

void Engine::finish(double dt, std::vector<Vertex>& out) {
  out.clear();
  animating_ = false;
  placed_ = 0;
  auto blocked = [&](const Rect& r) {
    if (!r.inside(view_)) return true;
    for (const auto& b : blockers_)
      if (b.overlaps(r)) return true;
    return false;
  };

  // icons that always draw go in first, so text keeps clear of every one of them
  for (const auto& l : points_) {
    if (!l.icon || !l.icon_overlap || l.opacity <= 0.003f) continue;
    const float r = l.icon->radius + l.icon->halo_px;
    const Rect box{l.x - r, l.y - r, l.x + r, l.y + r};
    if (box.overlaps(view_)) index_.insert(box);
  }

  std::vector<Item> items;
  items.reserve(points_.size() + lines_.size());
  for (size_t i = 0; i < points_.size(); ++i) items.push_back({false, i, points_[i].priority});
  for (size_t i = 0; i < lines_.size(); ++i) items.push_back({true, i, lines_[i].priority});
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.priority < b.priority; });

  std::vector<Vertex> icons;  // icons draw under all text
  std::vector<Rect> boxes;
  std::unordered_map<std::string, bool> done;  // a parent tile standing in can repeat a label
  for (const auto& it : items) {
    if (!it.line) {
      const auto& l = points_[it.index];
      if (l.opacity <= 0.003f || !done.emplace(l.key, true).second) continue;
      const Run run = l.text.empty() ? Run{} : shape(atlas_, l.style, l.text);
      const float baseline_off = l.style.size * 0.34f;
      bool icon_ok = true;
      Rect icon_box{};
      if (l.icon) {
        const float r = l.icon->radius + l.icon->halo_px;
        icon_box = {l.x - r, l.y - r, l.x + r, l.y + r};
        if (!l.icon_overlap) icon_ok = !blocked(icon_box) && index_.free(icon_box);
      }
      // text positions to try: centred without an icon, right then left of one
      float tx = 0, ty = l.y + baseline_off;
      bool text_ok = false;
      if (!l.text.empty() && icon_ok) {
        std::vector<float> xs;
        if (l.icon) {
          const float gap = l.icon->radius + 4;
          xs = {l.x + gap, l.x - gap - run.width};
        } else {
          xs = {l.x - run.width / 2};
        }
        tx = xs[0];
        for (float x : xs) {
          const Rect b = text_box(x, ty, run, l.style.size);
          if (!blocked(b) && index_.free(b)) {
            tx = x;
            text_ok = true;
            break;
          }
        }
      }
      if (l.icon && !l.text_optional && !l.text.empty() && !text_ok) icon_ok = l.icon_overlap;
      if (icon_ok && l.icon && !l.icon_overlap) index_.insert(icon_box);
      if (text_ok) {
        index_.insert(text_box(tx, ty, run, l.style.size));
        ++placed_;
      }
      const float a = l.text.empty() ? 0 : step(l.key, text_ok, dt);
      if (icon_ok && l.icon) {
        emit_point(l, run, tx, ty, 0, false, true, icons);
        if (l.pick >= 0) hits_.push_back({icon_box, l.pick});
      }
      if (!l.text.empty()) emit_point(l, run, tx, ty, a, true, false, out);
      continue;
    }

    const auto& l = lines_[it.index];
    if (l.opacity <= 0.003f || l.pts.size() < 2 || l.text.empty()) continue;
    std::vector<float> cum{0};
    for (size_t i = 1; i < l.pts.size(); ++i)
      cum.push_back(cum.back() + std::hypot(l.pts[i][0] - l.pts[i - 1][0], l.pts[i][1] - l.pts[i - 1][1]));
    const float total = cum.back();
    const Run run = shape(atlas_, l.style, l.text);
    if (run.width + 8 > total) continue;
    // the reversed line, for anchors where the forward direction would put the text upside down
    LineLabel rev = l;
    std::reverse(rev.pts.begin(), rev.pts.end());
    std::vector<float> rcum{0};
    for (size_t i = 1; i < rev.pts.size(); ++i)
      rcum.push_back(rcum.back() + std::hypot(rev.pts[i][0] - rev.pts[i - 1][0], rev.pts[i][1] - rev.pts[i - 1][1]));

    auto point_at = [&](float d) {
      size_t s = 0;
      while (s + 2 < cum.size() && cum[s + 1] < d) ++s;
      const float sl = cum[s + 1] - cum[s], t = sl > 0 ? (d - cum[s]) / sl : 0;
      return std::array<float, 2>{l.pts[s][0] + (l.pts[s + 1][0] - l.pts[s][0]) * t,
                                  l.pts[s][1] + (l.pts[s + 1][1] - l.pts[s][1]) * t};
    };
    std::vector<float> anchors;
    if (total < l.spacing) {
      anchors.push_back(total / 2);
    } else {
      for (float d = l.spacing / 2; d < total; d += l.spacing) anchors.push_back(d);
    }
    for (size_t ai = 0; ai < anchors.size(); ++ai) {
      const float d = anchors[ai];
      const auto ap = point_at(d);
      if (ap[0] < l.owner.x0 || ap[0] >= l.owner.x1 || ap[1] < l.owner.y0 || ap[1] >= l.owner.y1) continue;
      if (ap[0] < 0 || ap[1] < 0 || ap[0] > w_ || ap[1] > h_) continue;
      const std::string key = l.key + "#" + std::to_string(ai);
      // same text nearby (the same line crossing into the next tile) -> skip
      bool near = false;
      for (const auto& q : line_anchors_[l.text])
        if (std::hypot(q[0] - ap[0], q[1] - ap[1]) < l.spacing * 0.6f) {
          near = true;
          break;
        }
      // keep the text upright: read left to right
      const auto a0 = point_at(std::max(0.0f, d - run.width / 2)), a1 = point_at(std::min(total, d + run.width / 2));
      const bool flip = a1[0] < a0[0];
      const LineLabel& use = flip ? rev : l;
      const auto& ucum = flip ? rcum : cum;
      const float ud = flip ? total - d : d;
      bool ok = !near && place_line(use, run, ai, ud, ucum, nullptr, 1, boxes);
      if (ok)
        for (const auto& b : boxes)
          if (blocked(b) || !index_.free(b)) {
            ok = false;
            break;
          }
      const float a = step(key, ok, dt);
      if (ok) {
        for (const auto& b : boxes) index_.insert(b);
        line_anchors_[l.text].push_back(ap);
        ++placed_;
      }
      if (a > 0.003f) place_line(use, run, ai, ud, ucum, &out, a, boxes);
    }
  }
  out.insert(out.begin(), icons.begin(), icons.end());

  // forget labels that left the view
  for (auto it = fades_.begin(); it != fades_.end();)
    it = it->second.seen + 2 < frame_ ? fades_.erase(it) : std::next(it);
}

int Engine::hit(float x, float y, float slop) const {
  for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
    const Rect& r = it->first;
    if (x >= r.x0 - slop && x <= r.x1 + slop && y >= r.y0 - slop && y <= r.y1 + slop) return it->second;
  }
  return -1;
}

}  // namespace gsa::text
