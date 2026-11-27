// Web Mercator maths. World coordinates run 0..1 in x (west to east) and y (north to south).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gsa::geo {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTilePx = 512.0;  // screen pixels per tile at integer zoom (same as the web prototype)

inline double mx(double lon) { return (lon + 180.0) / 360.0; }
inline double my(double lat) {
  const double s = std::sin(std::clamp(lat, -85.05112878, 85.05112878) * kPi / 180.0);
  return 0.5 - std::log((1 + s) / (1 - s)) / (4 * kPi);
}
inline double lon_of(double x) { return x * 360.0 - 180.0; }
inline double lat_of(double y) {
  const double n = kPi - 2.0 * kPi * y;
  return 180.0 / kPi * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

struct TileId {
  int z = 0;
  uint32_t x = 0, y = 0;
  bool operator==(const TileId&) const = default;
  bool operator<(const TileId& o) const {
    if (z != o.z) return z < o.z;
    if (x != o.x) return x < o.x;
    return y < o.y;
  }
  TileId parent() const { return z == 0 ? *this : TileId{z - 1, x / 2, y / 2}; }
  double size() const { return 1.0 / double(1u << z); }
  double ox() const { return x * size(); }
  double oy() const { return y * size(); }
};

struct TileIdHash {
  size_t operator()(const TileId& t) const { return (size_t(t.z) << 58) ^ (size_t(t.x) << 29) ^ size_t(t.y); }
};

struct Camera {
  double cx = mx(25.2), cy = my(-28.8);  // centre, world units
  double zoom = 5.4;
  double w = 1280, h = 800;            // viewport in logical pixels

  double scale() const { return kTilePx * std::pow(2.0, zoom); }  // pixels per world unit
  void to_screen(double x, double y, double& sx, double& sy) const {
    sx = (x - cx) * scale() + w / 2;
    sy = (y - cy) * scale() + h / 2;
  }
  void to_world(double sx, double sy, double& x, double& y) const {
    x = (sx - w / 2) / scale() + cx;
    y = (sy - h / 2) / scale() + cy;
  }
  // Zoom by dz keeping the world point under (sx, sy) fixed.
  void zoom_at(double dz, double sx, double sy, double zmin = 3.0, double zmax = 16.0) {
    double wx, wy;
    to_world(sx, sy, wx, wy);
    zoom = std::clamp(zoom + dz, zmin, zmax);
    cx = wx - (sx - w / 2) / scale();
    cy = wy - (sy - h / 2) / scale();
  }
  void pan_px(double dx, double dy) {
    cx -= dx / scale();
    cy = std::clamp(cy - dy / scale(), 0.0, 1.0);
  }
};

// Tiles at zoom tz covering the viewport, nearest to the centre first.
inline std::vector<TileId> cover(const Camera& c, int tz) {
  const double n = double(1u << tz);
  double x0, y0, x1, y1;
  c.to_world(0, 0, x0, y0);
  c.to_world(c.w, c.h, x1, y1);
  const int tx0 = std::max(0, int(std::floor(x0 * n))), tx1 = std::min(int(n) - 1, int(std::floor(x1 * n)));
  const int ty0 = std::max(0, int(std::floor(y0 * n))), ty1 = std::min(int(n) - 1, int(std::floor(y1 * n)));
  std::vector<TileId> out;
  for (int x = tx0; x <= tx1; ++x)
    for (int y = ty0; y <= ty1; ++y) out.push_back({tz, uint32_t(x), uint32_t(y)});
  const double ccx = c.cx * n, ccy = c.cy * n;
  std::sort(out.begin(), out.end(), [&](const TileId& a, const TileId& b) {
    const double da = std::hypot(a.x + 0.5 - ccx, a.y + 0.5 - ccy), db = std::hypot(b.x + 0.5 - ccx, b.y + 0.5 - ccy);
    return da < db;
  });
  return out;
}

}  // namespace gsa::geo
