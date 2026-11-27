// Label placement for the map: shaping, collision, fades and curved labels along lines.
//
// Each frame the map hands the engine candidates in screen pixels (point labels with an
// optional icon, and line labels). The engine places them in priority order so none overlap
// each other or the interface, fades them in and out, and writes textured quads for the
// text pipeline.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "atlas.hpp"

namespace gsa::text {

struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool overlaps(const Rect& o) const { return x0 < o.x1 && x1 > o.x0 && y0 < o.y1 && y1 > o.y0; }
  bool inside(const Rect& o) const { return x0 >= o.x0 && y0 >= o.y0 && x1 <= o.x1 && y1 <= o.y1; }
};

// One textured corner of a glyph or icon quad, in logical screen pixels.
struct Vertex {
  float x, y, u, v;
  uint32_t fill;    // RGBA8, alpha includes the fade
  uint32_t halo;    // RGBA8
  float halo_w;     // halo reach in distance-field units (0..1 of the texel range)
  float gamma;      // edge softness multiplier, 1 = default
};
static_assert(sizeof(Vertex) == 32);

struct Style {
  int font = 0;
  float size = 12;          // pixels
  uint32_t fill = 0xffffffff;
  uint32_t halo = 0xff000000;
  float halo_px = 1.5f;
  float spacing_em = 0;     // extra letter spacing
};

struct IconSpec {
  Icon kind = Icon::Square;
  float radius = 4;         // pixels
  uint32_t fill = 0xffffffff;
  uint32_t halo = 0xff000000;
  float halo_px = 1.2f;
};

struct PointLabel {
  std::string key;          // stable identity for fades
  std::string text;         // may be empty for an icon alone
  float x = 0, y = 0;       // anchor, screen pixels
  Style style;
  int priority = 0;         // lower places first
  float opacity = 1;        // zoom fade of the feature itself
  std::optional<IconSpec> icon;
  bool icon_overlap = true; // icons always draw (they still block text)
  bool text_optional = true;  // with an icon: drop the text and keep the icon when the text has no room
};

struct LineLabel {
  std::string key;
  std::string text;
  std::vector<std::array<float, 2>> pts;  // screen pixels
  Rect owner;               // anchors must fall inside (the tile that owns this geometry)
  Style style;
  int priority = 0;
  float opacity = 1;
  float spacing = 420;      // pixels between repeats along the line
  float max_bend_deg = 40;  // largest turn between neighbouring glyphs
};

// Uniform grid of boxes.
class CollisionIndex {
 public:
  void reset(float w, float h, float cell = 64);
  bool free(const Rect& r) const;
  void insert(const Rect& r);
  size_t size() const { return boxes_.size(); }

 private:
  void cells(const Rect& r, int& cx0, int& cy0, int& cx1, int& cy1) const;
  float cell_ = 64;
  int cols_ = 0, rows_ = 0;
  std::vector<Rect> boxes_;
  std::vector<std::vector<uint32_t>> grid_;
};

struct ShapedGlyph {
  Glyph g;
  float x;    // pen position of the glyph, pixels from the run's start
  float adv;  // pixels
};

struct Run {
  std::vector<ShapedGlyph> glyphs;
  float width = 0;      // pixels
  float ascent = 0, descent = 0;
};

Run shape(Atlas& atlas, const Style& st, const std::string& text);

class Engine {
 public:
  explicit Engine(Atlas& atlas) : atlas_(atlas) {}

  // Starts a frame: the viewport and the rectangles covered by the interface.
  void begin(float w, float h, const std::vector<Rect>& blockers);
  void add(PointLabel l) { points_.push_back(std::move(l)); }
  void add(LineLabel l) { lines_.push_back(std::move(l)); }
  // Places everything added since begin(), advances fades by dt seconds and writes quads.
  // dt < 0 skips fades (labels appear at once; used for screenshots).
  void finish(double dt, std::vector<Vertex>& out);

  bool animating() const { return animating_; }
  size_t placed() const { return placed_; }
  // Boxes the labels and icons of the last frame took, for tests and debugging.
  const CollisionIndex& collisions() const { return index_; }

 private:
  struct Fade {
    float a = 0;
    uint64_t seen = 0;
  };
  struct Item {
    bool line;
    size_t index;
    int priority;
  };
  float step(const std::string& key, bool placed, double dt);
  void emit_point(const PointLabel& l, const Run& run, float tx, float ty, float a, bool with_text, bool with_icon,
                  std::vector<Vertex>& out);
  bool place_line(const LineLabel& l, const Run& run, size_t anchor_index, float anchor_d,
                  const std::vector<float>& cum, std::vector<Vertex>* out, float a, std::vector<Rect>& boxes);

  Atlas& atlas_;
  float w_ = 0, h_ = 0;
  Rect view_;
  std::vector<Rect> blockers_;
  std::vector<PointLabel> points_;
  std::vector<LineLabel> lines_;
  CollisionIndex index_;
  std::unordered_map<std::string, Fade> fades_;
  std::unordered_map<std::string, std::vector<std::array<float, 2>>> line_anchors_;
  uint64_t frame_ = 0;
  bool animating_ = false;
  size_t placed_ = 0;
};

// Packs straight RGBA (0..255) into the vertex colour format; `a` scales alpha.
uint32_t rgba(uint32_t packed, float a);

}  // namespace gsa::text
