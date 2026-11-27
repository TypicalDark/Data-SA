#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "map_labels.hpp"
#include "text/atlas.hpp"
#include "text/labels.hpp"

using namespace gsa;
using namespace gsa::text;

namespace {

int load_regular(Atlas& a) { return a.add_font(std::string(GSA_ASSETS) + "/fonts/SpaceGrotesk-Regular.ttf"); }

Style test_style(int font) {
  Style s;
  s.font = font;
  s.size = 12;
  s.fill = 0xffffffffu;
  s.halo = 0xff000000u;
  return s;
}

float max_alpha(const std::vector<Vertex>& v) {
  uint32_t a = 0;
  for (const auto& x : v) a = std::max(a, x.fill >> 24);
  return float(a) / 255.0f;
}

}  // namespace

TEST_CASE("utf8 decoding") {
  const auto cps = utf8_decode("Naud\xc3\xa9");
  REQUIRE(cps.size() == 5);
  CHECK(cps[4] == 0xe9);
  CHECK(utf8_decode("a\xff" "b") == std::vector<uint32_t>{'a', 0xfffd, 'b'});
  CHECK(utf8_decode("\xe2\x82") == std::vector<uint32_t>{0xfffd});  // cut short
  CHECK(utf8_decode("!Kheis").size() == 6);
}

TEST_CASE("glyph atlas renders distance fields on demand") {
  Atlas a;
  CHECK(a.add_font("/no/such/font.ttf") == -1);
  const int f = load_regular(a);
  REQUIRE(f == 0);
  CHECK(a.ascent(f) > 0);
  CHECK(a.descent(f) < 0);

  const Glyph& g = a.glyph(f, 'A');
  REQUIRE(g.valid);
  CHECK(g.advance > 8);
  CHECK(g.x1 - g.x0 > 2 * Atlas::kPad);
  CHECK(g.u1 > g.u0);
  CHECK(g.y0 < 0);  // above the baseline
  // the field is on-edge or more somewhere inside the glyph, and empty well outside it
  const auto& px = a.pixels();
  const int x0 = int(g.u0 * Atlas::kSize), y0 = int(g.v0 * Atlas::kSize);
  const int x1 = int(g.u1 * Atlas::kSize), y1 = int(g.v1 * Atlas::kSize);
  uint8_t hi = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) hi = std::max(hi, px[size_t(y) * Atlas::kSize + size_t(x)]);
  CHECK(hi >= Atlas::kOnEdge);
  CHECK(px[size_t(y0) * Atlas::kSize + size_t(x0)] < Atlas::kOnEdge);

  const Glyph& sp = a.glyph(f, ' ');
  CHECK(sp.valid);
  CHECK(sp.advance > 0);
  CHECK(sp.x1 == sp.x0);  // nothing to draw

  // the same glyph twice is stored once
  const float u = a.glyph(f, 'A').u0;
  CHECK(a.glyph(f, 'A').u0 == u);
  // a code point the font lacks falls back to '?'
  CHECK(a.glyph(f, 0x4e2d).advance == a.glyph(f, '?').advance);
}

TEST_CASE("icon distance fields are inside at the centre and outside at the corner") {
  Atlas a;
  for (Icon k : {Icon::Square, Icon::Circle, Icon::Diamond, Icon::Triangle}) {
    const Glyph& g = a.icon(k);
    REQUIRE(g.valid);
    const auto& px = a.pixels();
    const int x0 = int(g.u0 * Atlas::kSize), y0 = int(g.v0 * Atlas::kSize);
    const int x1 = int(g.u1 * Atlas::kSize), y1 = int(g.v1 * Atlas::kSize);
    const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    CHECK(px[size_t(cy) * Atlas::kSize + size_t(cx)] > Atlas::kOnEdge);
    CHECK(px[size_t(y0 + 1) * Atlas::kSize + size_t(x0 + 1)] < Atlas::kOnEdge);
    CHECK(g.x0 == doctest::Approx(-(Atlas::kIconRadius + Atlas::kPad)));
  }
  // a circle reaches the middle of its sides, a diamond its tips only
  const Glyph& c = a.icon(Icon::Circle);
  const int cx = int((c.u0 + c.u1) / 2 * Atlas::kSize), cy = int((c.v0 + c.v1) / 2 * Atlas::kSize);
  const size_t edge = size_t(cy) * Atlas::kSize + size_t(cx + Atlas::kIconRadius);
  CHECK(std::abs(int(a.pixels()[edge]) - int(Atlas::kOnEdge)) < 30);
}

TEST_CASE("shaping lays glyphs left to right") {
  Atlas a;
  const int f = load_regular(a);
  Style s = test_style(f);
  const Run r = shape(a, s, "Cape Town");
  REQUIRE(r.glyphs.size() == 9);
  CHECK(r.width > 40);
  CHECK(r.width < 80);
  for (size_t i = 1; i < r.glyphs.size(); ++i) CHECK(r.glyphs[i].x > r.glyphs[i - 1].x);
  s.size = 24;
  CHECK(shape(a, s, "Cape Town").width == doctest::Approx(r.width * 2).epsilon(0.01));
  s.spacing_em = 0.1f;
  CHECK(shape(a, s, "Cape Town").width > r.width * 2 + 8 * 2);
}

TEST_CASE("collision index") {
  CollisionIndex c;
  c.reset(800, 600, 64);
  CHECK(c.free({10, 10, 50, 30}));
  c.insert({10, 10, 50, 30});
  CHECK_FALSE(c.free({40, 20, 90, 40}));
  CHECK(c.free({51, 10, 90, 30}));
  c.insert({700, 500, 900, 700});  // partly off screen
  CHECK_FALSE(c.free({790, 590, 795, 595}));
  CHECK(c.size() == 2);
}

TEST_CASE("point labels: priority wins, the interface blocks, fades run") {
  Atlas a;
  const int f = load_regular(a);
  Engine e(a);
  std::vector<Vertex> out;

  auto label = [&](const char* key, int pri, float x) {
    PointLabel l;
    l.key = key;
    l.text = key;
    l.x = x;
    l.y = 300;
    l.style = test_style(f);
    l.priority = pri;
    return l;
  };
  e.begin(800, 600, {});
  e.add(label("Second", 5, 402));
  e.add(label("First", 1, 400));
  e.finish(-1, out);
  CHECK(e.placed() == 1);
  CHECK(out.size() == 5 * 6);  // "First": five glyph quads
  CHECK(max_alpha(out) == doctest::Approx(1));

  // a panel over the label hides it
  e.begin(800, 600, {{350, 250, 450, 350}});
  e.add(label("First", 1, 400));
  e.finish(-1, out);
  CHECK(e.placed() == 0);
  CHECK(out.empty());

  // fade in over 0.2 s
  Engine g(a);
  g.begin(800, 600, {});
  g.add(label("Fade", 1, 400));
  g.finish(0.1, out);
  CHECK(max_alpha(out) == doctest::Approx(0.5).epsilon(0.02));
  CHECK(g.animating());
  g.begin(800, 600, {});
  g.add(label("Fade", 1, 400));
  g.finish(0.15, out);
  CHECK(max_alpha(out) == doctest::Approx(1));
  CHECK_FALSE(g.animating());

  // a label off the edge of the window is not placed
  e.begin(800, 600, {});
  e.add(label("Edge", 1, 798));
  e.finish(-1, out);
  CHECK(e.placed() == 0);
}

TEST_CASE("icons always draw and push text aside") {
  Atlas a;
  const int f = load_regular(a);
  Engine e(a);
  std::vector<Vertex> out;
  PointLabel l;
  l.key = "sub";
  l.text = "Apollo";
  l.x = 400;
  l.y = 300;
  l.style = test_style(f);
  l.icon = IconSpec{Icon::Square, 5, 0xff0000ffu, 0xff000000u, 1.2f};
  // another icon right of the first, where the text would go
  PointLabel m = l;
  m.key = "sub2";
  m.text.clear();
  m.x = 430;
  e.begin(800, 600, {});
  e.add(l);
  e.add(m);
  e.finish(-1, out);
  CHECK(e.placed() == 1);
  // two icon quads first, then "Apollo" to the left of the first icon
  REQUIRE(out.size() == 2 * 6 + 6 * 6);
  float maxx = 0;
  for (size_t i = 12; i < out.size(); ++i) maxx = std::max(maxx, out[i].x);
  CHECK(maxx < 400);
}

TEST_CASE("line labels follow the line, stay upright and skip sharp bends") {
  Atlas a;
  const int f = load_regular(a);
  Engine e(a);
  std::vector<Vertex> out;
  LineLabel l;
  l.key = "line";
  l.text = "400 kV";
  l.style = test_style(f);
  l.owner = {0, 0, 800, 600};
  l.spacing = 2000;

  // drawn right to left: the text must still read left to right
  l.pts = {{700, 300}, {100, 300}};
  e.begin(800, 600, {});
  e.add(l);
  e.finish(-1, out);
  CHECK(e.placed() == 1);
  REQUIRE(out.size() == 5 * 6);  // the space has no quad
  CHECK(out.front().x < out.back().x);
  for (const auto& v : out) CHECK(std::abs(v.y - 300) < 14);

  // a diagonal: glyphs step down the slope
  l.pts = {{100, 100}, {500, 500}};
  e.begin(800, 600, {});
  e.add(l);
  e.finish(-1, out);
  CHECK(e.placed() == 1);
  CHECK(out.back().y > out.front().y + 20);

  // a zigzag with right-angle turns every 8 pixels cannot carry text
  l.pts.clear();
  for (int i = 0; i < 40; ++i) l.pts.push_back({float(200 + (i / 2) * 8), float(300 + (i % 2 ? 8 : 0) * ((i / 2) % 2 ? 1 : -1))});
  l.max_bend_deg = 40;
  e.begin(800, 600, {});
  e.add(l);
  e.finish(-1, out);
  CHECK(e.placed() == 0);

  // an anchor outside the owning tile belongs to the neighbour
  l.pts = {{100, 300}, {700, 300}};
  l.owner = {0, 0, 300, 600};
  e.begin(800, 600, {});
  e.add(l);
  e.finish(-1, out);
  CHECK(e.placed() == 0);
}

TEST_CASE("map label fade gates") {
  CHECK(map_labels::fade(5.0, 6.6) == 0);
  CHECK(map_labels::fade(6.6, 6.6) == 1);
  CHECK(map_labels::fade(6.25, 6.6) == doctest::Approx(0.5));
}
