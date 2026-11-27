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
