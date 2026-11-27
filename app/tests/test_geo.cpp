#include <doctest/doctest.h>

#include "geo.hpp"
#include "style.hpp"

using namespace gsa;

TEST_CASE("mercator round trip") {
  for (double lat : {-34.0, -28.8, 0.0, 45.0})
    CHECK(geo::lat_of(geo::my(lat)) == doctest::Approx(lat).epsilon(1e-9));
  CHECK(geo::lon_of(geo::mx(18.42)) == doctest::Approx(18.42));
}

TEST_CASE("zoom at cursor keeps the point under the cursor") {
  geo::Camera c;
  c.w = 1000, c.h = 800;
  double x0, y0, x1, y1;
  c.to_world(300, 200, x0, y0);
  c.zoom_at(1.3, 300, 200);
  c.to_world(300, 200, x1, y1);
  CHECK(x1 == doctest::Approx(x0));
  CHECK(y1 == doctest::Approx(y0));
}

TEST_CASE("tile cover of the default view") {
  geo::Camera c;
  c.w = 1600, c.h = 1000;
  const auto t = geo::cover(c, 5);
  REQUIRE(!t.empty());
  CHECK(t.size() <= 12);
  // nearest to the centre first: South Africa's middle is in z5 tile x=18, y=18
  CHECK(t[0].x == 18);
  CHECK(t[0].y == 18);
}

TEST_CASE("topic colours") {
  mvt::Feature f;
  f.props["density"] = 500.0;
  f.props["value_median"] = 1.5e6;
  f.props["parcels"] = int64_t(100);
  f.props["win"] = std::string("DA");
  CHECK(style::muni_fill(style::Topic::People, f)->packed() == style::hex("#f8a77a", 0.78f).packed());
  CHECK(style::muni_fill(style::Topic::Elections, f)->packed() == style::hex("#2f78d6", 0.78f).packed());
  CHECK(style::muni_fill(style::Topic::Property, f)->a < style::hex("#fff", 0.78f).a);  // paler under 500 parcels
  mvt::Feature empty;
  CHECK_FALSE(style::muni_fill(style::Topic::People, empty).has_value());
  CHECK(style::volt(400).packed() == style::hex("#b0207e").packed());
  CHECK(style::line_visible(400, 3.5));
  CHECK_FALSE(style::line_visible(88, 5.0));
}

TEST_CASE("tilted view: screen and ground points round-trip") {
  geo::Camera c;
  c.w = 1600;
  c.h = 1000;
  c.zoom = 14;
  c.cx = geo::mx(28.05);
  c.cy = geo::my(-26.1);
  for (double pitch : {0.0, 30.0, 60.0}) {
    c.pitch = pitch;
    for (auto [sx, sy] : {std::pair{800.0, 500.0}, {10.0, 20.0}, {1590.0, 990.0}, {400.0, 700.0}}) {
      double x, y, bx, by;
      c.to_world(sx, sy, x, y);
      c.to_screen(x, y, bx, by);
      CHECK(bx == doctest::Approx(sx).epsilon(1e-6));
      CHECK(by == doctest::Approx(sy).epsilon(1e-6));
    }
    // the centre stays put, and land at the top of the screen is further off when tilted
    double x, y;
    c.to_world(800, 500, x, y);
    CHECK(x == doctest::Approx(c.cx));
    CHECK(y == doctest::Approx(c.cy));
  }
  geo::Camera flat = c;
  flat.pitch = 0;
  double fy, ty, x;
  flat.to_world(800, 0, x, fy);
  c.pitch = 60;
  c.to_world(800, 0, x, ty);
  CHECK(ty < fy);
  CHECK(geo::cover(c, 14).size() > geo::cover(flat, 14).size());
  // zooming at a point keeps it under the pointer when tilted
  double wx, wy, ax, ay;
  c.to_world(300, 200, wx, wy);
  c.zoom_at(0.5, 300, 200);
  c.to_world(300, 200, ax, ay);
  CHECK(ax == doctest::Approx(wx).epsilon(1e-9));
  CHECK(ay == doctest::Approx(wy).epsilon(1e-9));
}
