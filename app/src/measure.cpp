#include "measure.hpp"

#include <cmath>
#include <cstdio>

namespace gsa::measure {

namespace {
constexpr double kR = 6371008.8, kPi = 3.14159265358979323846, kRad = kPi / 180;

std::string fmt1(double v) {
  // one decimal under 100, none above (the website's fmt1)
  char b[32];
  std::snprintf(b, sizeof b, v < 100 ? "%.1f" : "%.0f", v);
  return b;
}
}  // namespace

double metres(const LonLat& a, const LonLat& b) {
  const double la1 = a[1] * kRad, la2 = b[1] * kRad, dl = (b[0] - a[0]) * kRad;
  const double s = std::pow(std::sin((la2 - la1) / 2), 2) + std::cos(la1) * std::cos(la2) * std::pow(std::sin(dl / 2), 2);
  return 2 * kR * std::asin(std::sqrt(s));
}

double bearing(const LonLat& a, const LonLat& b) {
  const double la1 = a[1] * kRad, la2 = b[1] * kRad, dl = (b[0] - a[0]) * kRad;
  const double y = std::sin(dl) * std::cos(la2), x = std::cos(la1) * std::sin(la2) - std::sin(la1) * std::cos(la2) * std::cos(dl);
  return std::fmod(std::atan2(y, x) / kRad + 360, 360);
}

double length(const std::vector<LonLat>& p) {
  double s = 0;
  for (size_t i = 1; i < p.size(); ++i) s += metres(p[i - 1], p[i]);
  return s;
}

double area_m2(const std::vector<LonLat>& r) {
  double s = 0;
  for (size_t i = 0; i < r.size(); ++i) {
    const auto& a = r[i];
    const auto& b = r[(i + 1) % r.size()];
    s += (b[0] - a[0]) * kRad * (2 + std::sin(a[1] * kRad) + std::sin(b[1] * kRad));
  }
  return std::abs(s * kR * kR / 2);
}

std::string fmt_dist(double m) {
  char b[32];
  if (m >= 10000) return fmt1(m / 1000) + " km";
  if (m >= 1000) {
    std::snprintf(b, sizeof b, "%.2f km", m / 1000);
    return b;
  }
  std::snprintf(b, sizeof b, "%d m", int(std::lround(m)));
  return b;
}

std::string fmt_area(double m2) {
  if (m2 >= 1e6) return fmt1(m2 / 1e6) + " km\xc2\xb2";
  if (m2 >= 1e4) return fmt1(m2 / 1e4) + " ha";
  return std::to_string(std::lround(m2)) + " m\xc2\xb2";
}

std::string summary(Mode mode, const std::vector<LonLat>& p) {
  if (mode == Mode::Circle && p.size() == 2) {
    const double r = metres(p[0], p[1]);
    return "Radius " + fmt_dist(r) + " \xc2\xb7 area " + fmt_area(kPi * r * r);
  }
  if (mode == Mode::Area && p.size() >= 3) {
    auto ring = p;
    ring.push_back(p[0]);
    return fmt_area(area_m2(p)) + " \xc2\xb7 perimeter " + fmt_dist(length(ring));
  }
  return fmt_dist(length(p));
}

std::string hint(Mode mode, size_t n) {
  if (mode == Mode::Circle) return n ? "Click to set the radius" : "Click the centre";
  if (n == 0) return "Click to start";
  if (mode == Mode::Area) return "Keep clicking \xc2\xb7 click the first point to close";
  return "Keep clicking \xc2\xb7 Enter or click the last point to finish";
}

std::string geojson(Mode mode, const std::vector<LonLat>& p) {
  auto pt = [](const LonLat& c) {
    char b[64];
    std::snprintf(b, sizeof b, "[%.6f,%.6f]", c[0], c[1]);
    return std::string(b);
  };
  std::string coords;
  for (const auto& c : p) coords += (coords.empty() ? "" : ",") + pt(c);
  if (mode == Mode::Area && p.size() >= 3)
    return R"({"type":"Feature","properties":{"area_m2":)" + std::to_string(std::lround(area_m2(p))) +
           R"(},"geometry":{"type":"Polygon","coordinates":[[)" + coords + "," + pt(p[0]) + "]]}}";
  if (mode == Mode::Circle && p.size() == 2)
    return R"({"type":"Feature","properties":{"radius_m":)" + std::to_string(std::lround(metres(p[0], p[1]))) +
           R"(},"geometry":{"type":"Point","coordinates":)" + pt(p[0]) + "}}";
  return R"({"type":"Feature","properties":{"length_m":)" + std::to_string(std::lround(length(p))) +
         R"(},"geometry":{"type":"LineString","coordinates":[)" + coords + "]}}";
}

}  // namespace gsa::measure
