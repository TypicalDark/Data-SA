// Measure v2 as on the website: a path, an area or a circle, with the website's distance and area
// formats. Along-roads routes and the elevation profile come with Reach (milestone F).
#pragma once

#include <array>
#include <string>
#include <vector>

namespace gsa::measure {

enum class Mode { Path, Area, Circle };
using LonLat = std::array<double, 2>;

double metres(const LonLat& a, const LonLat& b);          // great circle (haversine)
double bearing(const LonLat& a, const LonLat& b);         // degrees from north, 0..360
double length(const std::vector<LonLat>& pts);            // along the path
double area_m2(const std::vector<LonLat>& ring);          // spherical polygon area
std::string fmt_dist(double m);                            // 850 m, 1.25 km, 12.5 km
std::string fmt_area(double m2);                           // 900 m², 4.5 ha, 12.5 km²
// The one-line result once a measurement is finished ("12.5 km", "4.5 ha · perimeter 1.2 km" ...).
std::string summary(Mode mode, const std::vector<LonLat>& pts);
// What to do next while measuring ("Click to start", "Keep clicking · click the first point to close").
std::string hint(Mode mode, size_t points);
// GeoJSON of the measurement, for Copy.
std::string geojson(Mode mode, const std::vector<LonLat>& pts);

}  // namespace gsa::measure
