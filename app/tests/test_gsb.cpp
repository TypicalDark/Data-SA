#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "gsb.hpp"
#include "mesh.hpp"

using namespace gsa;

namespace {
void put_uv(std::string& s, uint64_t v) {
  do {
    uint8_t b = v & 127;
    v >>= 7;
    if (v) b |= 128;
    s.push_back(char(b));
  } while (v);
}
uint64_t zz(int64_t v) { return v < 0 ? uint64_t(-v) * 2 - 1 : uint64_t(v) * 2; }
}  // namespace

TEST_CASE("GSB1 tiles decode as the website does") {
  // one 10 m x 6 m house in the middle of a Johannesburg z12 tile, and one triangle polygon
  std::string b;
  put_uv(b, 1);  // rects
  put_uv(b, 1);  // polygons
  b.push_back(char(8));          // rect flags: house, height known
  put_uv(b, zz(8192));           // dx
  put_uv(b, zz(8192));           // dy
  b.push_back(char(20));         // L, metres x 2
  b.push_back(char(12));         // W
  b.push_back(char(0));          // angle
  put_uv(b, 14);                 // height 7 m
  b.push_back(char(2 | 4));      // polygon flags: work, machine-traced, no height
  put_uv(b, 3);                  // vertices
  put_uv(b, zz(1000));
  put_uv(b, zz(1000));
  put_uv(b, zz(400));
  put_uv(b, zz(0));
  put_uv(b, zz(0));
  put_uv(b, zz(400));
  const uint32_t tx = 2367, ty = 2353;
  const auto t = gsb::decode(b, tx, ty);
  const auto* l = t.layer("building");
  REQUIRE(l);
  REQUIRE(l->features.size() == 2);
  const auto& house = l->features[0];
  CHECK(house.num("h").value() == doctest::Approx(7));
  CHECK(house.num("k").value() == 0);
  REQUIRE(house.parts[0].size() == 4);
  float mx = 0, my = 0;
  for (const auto& p : house.parts[0]) {
    mx += p.x / 4;
    my += p.y / 4;
  }
  CHECK(mx == doctest::Approx(0.5).epsilon(0.001));
  CHECK(my == doctest::Approx(0.5).epsilon(0.002));
  // a z12 tile is about 8.7 km wide here, so 10 m is about 0.00115 of it
  const float w = house.parts[0][0].x - house.parts[0][2].x;
  CHECK(std::abs(w) == doctest::Approx(10.0 / 8730).epsilon(0.05));
  const auto& shed = l->features[1];
  CHECK(shed.parts[0].size() == 3);
  CHECK(shed.num("k").value() == 2);
  CHECK(shed.num("h").value() == 11);  // about 213 m sides: a big work building
  // and they become triangles
  mesh::Mesh m;
  layers::MapStyle ms;
  ms.buildings = true;
  mesh::build("buildings", t, ms, m, 12);
  CHECK(m.lines.size() == 6 + 3);

  CHECK_THROWS(gsb::decode(std::string("\x05\x05", 2), tx, ty));
}
