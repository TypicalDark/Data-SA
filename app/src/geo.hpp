// Web Mercator maths. World coordinates run 0..1 in x (west to east) and y (north to south).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
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

// The map view. With `pitch` (degrees, 0 = straight down, up to kMaxPitch) the ground tilts away
// around the screen centre: a camera `dist()` pixels above the centre looks at it, so the top of the
// screen shows land further off. "Flat" screen coordinates are where a point sits with no tilt; the
// renderer applies the tilt on the GPU with the same formula as tilt().
inline constexpr double kMaxPitch = 60.0;

struct Camera {
  double cx = mx(25.2), cy = my(-28.8);  // centre, world units
  double zoom = 5.4;
  double w = 1280, h = 800;            // viewport in logical pixels
  double pitch = 0;                    // degrees

  double scale() const { return kTilePx * std::pow(2.0, zoom); }  // pixels per world unit
  double dist() const { return 1.5 * h; }                         // camera height over the centre, pixels
  // Flat screen point -> tilted screen point; z is height above the ground in pixels.
  void tilt(double fx, double fy, double z, double& sx, double& sy) const {
    if (pitch <= 0 && z == 0) {
      sx = fx;
      sy = fy;
      return;
    }
    const double t = pitch * kPi / 180, c = std::cos(t), s = std::sin(t), D = dist();
    const double X = fx - w / 2, Y = fy - h / 2;
    const double Yp = Y * c - z * s, Zp = Y * s + z * c;
    const double k = D / std::max(1.0, D - Zp);
    sx = w / 2 + X * k;
    sy = h / 2 + Yp * k;
  }
  void to_flat(double x, double y, double& fx, double& fy) const {
    fx = (x - cx) * scale() + w / 2;
    fy = (y - cy) * scale() + h / 2;
  }
  void to_screen(double x, double y, double& sx, double& sy) const {
    double fx, fy;
    to_flat(x, y, fx, fy);
    tilt(fx, fy, 0, sx, sy);
  }
  // Screen point -> the ground point under it (above the horizon it clamps to far away).
  void to_world(double sx, double sy, double& x, double& y) const {
    double fx = sx, fy = sy;
    if (pitch > 0) {
      const double t = pitch * kPi / 180, c = std::cos(t), s = std::sin(t), D = dist();
      const double v = (sy - h / 2) / D;
      const double den = std::max(0.05, c + v * s);
      const double Y = v * D / den;
      const double k = D / (D - Y * s);
      fx = w / 2 + (sx - w / 2) / k;
      fy = h / 2 + Y;
    }
    x = (fx - w / 2) / scale() + cx;
    y = (fy - h / 2) / scale() + cy;
  }
  // Zoom by dz keeping the world point under (sx, sy) fixed.
  void zoom_at(double dz, double sx, double sy, double zmin = 3.0, double zmax = 16.0) {
    double wx, wy;
    to_world(sx, sy, wx, wy);
    zoom = std::clamp(zoom + dz, zmin, zmax);
    double nx, ny;
    to_world(sx, sy, nx, ny);
    cx += wx - nx;
    cy += wy - ny;
  }
  void pan_px(double dx, double dy) {
    // the ground point at the centre moves with the pointer
    double ax, ay, bx, by;
    to_world(w / 2, h / 2, ax, ay);
    to_world(w / 2 - dx, h / 2 - dy, bx, by);
    cx += bx - ax;
    cy = std::clamp(cy + by - ay, 0.0, 1.0);
  }
};

// Tiles at zoom tz covering the viewport, nearest to the centre first.
inline std::vector<TileId> cover(const Camera& c, int tz) {
  const double n = double(1u << tz);
  double x0 = 1, y0 = 1, x1 = 0, y1 = 0;
  for (const auto& [sx, sy] : {std::pair{0.0, 0.0}, {c.w, 0.0}, {0.0, c.h}, {c.w, c.h}}) {
    double x, y;
    c.to_world(sx, sy, x, y);
    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
  }
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
