#include <doctest/doctest.h>

#include "measure.hpp"

using namespace gsa::measure;

TEST_CASE("measure: distances, bearings, areas, formats") {
  const LonLat cpt{18.4241, -33.9249}, jhb{28.0473, -26.2041};
  CHECK(metres(cpt, jhb) == doctest::Approx(1262000).epsilon(0.01));  // about 1 262 km
  CHECK(bearing({0, 0}, {0, 1}) == doctest::Approx(0));
  CHECK(bearing({0, 0}, {1, 0}) == doctest::Approx(90));
  // a 0.01 degree square at the equator is about 1.11 km on a side
  const std::vector<LonLat> sq{{0, 0}, {0.01, 0}, {0.01, 0.01}, {0, 0.01}};
  CHECK(area_m2(sq) == doctest::Approx(1.2364e6).epsilon(0.01));
  CHECK(fmt_dist(850) == "850 m");
  CHECK(fmt_dist(1250) == "1.25 km");
  CHECK(fmt_dist(12500) == "12.5 km");
  CHECK(fmt_dist(1262000) == "1262 km");
  CHECK(fmt_area(900) == "900 m\xc2\xb2");
  CHECK(fmt_area(45000) == "4.5 ha");
  CHECK(fmt_area(1.2364e6) == "1.2 km\xc2\xb2");
  CHECK(summary(Mode::Area, sq).rfind("1.2 km\xc2\xb2 \xc2\xb7 perimeter 4.45 km", 0) == 0);
  CHECK(summary(Mode::Circle, {{0, 0}, {0.01, 0}}).rfind("Radius 1.11 km", 0) == 0);
  CHECK(hint(Mode::Path, 0) == "Click to start");
  CHECK(geojson(Mode::Area, sq).find("\"Polygon\"") != std::string::npos);
}
