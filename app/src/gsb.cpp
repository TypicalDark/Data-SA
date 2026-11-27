#include "gsb.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace gsa::gsb {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double LQ = 2, PQ = 16384, AQ = 128;

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  uint8_t byte() {
    if (p >= end) throw std::runtime_error("GSB1 tile cut short");
    return *p++;
  }
  uint64_t uv() {
    uint64_t v = 0;
    int s = 0;
    uint8_t b;
    do {
      b = byte();
      v |= uint64_t(b & 127) << s;
      s += 7;
      if (s > 63) throw std::runtime_error("GSB1 varint too long");
    } while (b & 128);
    return v;
  }
  double sv() {
    const uint64_t v = uv();
    return (v & 1) ? -double((v + 1) / 2) : double(v / 2);
  }
};

double merc_y(double lat) {  // 0 at the top of the world, 1 at the bottom
  const double s = std::sin(lat * kPi / 180);
  return 0.5 - std::log((1 + s) / (1 - s)) / (4 * kPi);
}

}  // namespace

double estimate_height(double a, int k, bool ml) {
  if (k == 1) return 13;
  if (k == 2) return a < 200 ? 5 : a < 2000 ? 7 : a < 10000 ? 9 : 11;
  if (ml || k == 0) return a < 40 ? 2.8 : a < 250 ? 4 : a < 1000 ? 6 : a < 5000 ? 8 : 10;
  return a < 200 ? 4.5 : a < 2000 ? 7 : 9;
}

mvt::Tile decode(const std::string& bytes, uint32_t tx, uint32_t ty) {
  Reader r{reinterpret_cast<const uint8_t*>(bytes.data()), reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size()};
  const double n = 4096;
  const double lon0 = tx / n * 360 - 180, lon1 = (tx + 1) / n * 360 - 180;
  auto lat_of = [&](double t) { return std::atan(std::sinh(kPi * (1 - 2 * t / n))) * 180 / kPi; };
  const double lat0 = lat_of(ty + 1.0), lat1 = lat_of(double(ty));  // south, north
  const double kx = 111320 * std::cos((lat0 + lat1) / 2 * kPi / 180), ky = 110574;
  const double Wm = (lon1 - lon0) * kx, Hm = (lat1 - lat0) * ky;
  // metres east / north of the south-west corner -> tile units, y down
  auto pt = [&](double x, double y) {
    const double lat = lat0 + y / ky;
    return mvt::Point{float(x / Wm), float(merc_y(lat) * n - ty)};
  };

  const size_t nR = size_t(r.uv()), nP = size_t(r.uv());
  if (nR + nP > bytes.size() * 2) throw std::runtime_error("GSB1 counts too large");
  std::vector<uint8_t> fl(nR + nP);
  for (size_t i = 0; i < nR; ++i) fl[i] = r.byte();
  std::vector<double> cx(nR), cy(nR);
  double a = 0;
  for (size_t i = 0; i < nR; ++i) cx[i] = (a += r.sv()) / 16384 * Wm;
  a = 0;
  for (size_t i = 0; i < nR; ++i) cy[i] = (a += r.sv()) / 16384 * Hm;
  std::vector<uint8_t> L(nR), W(nR), A(nR);
  for (size_t i = 0; i < nR; ++i) L[i] = r.byte();
  for (size_t i = 0; i < nR; ++i) W[i] = r.byte();
  int ang = 0;
  for (size_t i = 0; i < nR; ++i) A[i] = uint8_t(ang = (ang + r.byte()) % int(AQ));
  std::vector<double> H(nR + nP);
  for (size_t i = 0; i < nR; ++i)
    H[i] = (fl[i] & 8) ? double(r.uv()) / 2 : estimate_height((L[i] / LQ) * (W[i] / LQ), fl[i] & 3, fl[i] & 4);
  for (size_t i = 0; i < nP; ++i) fl[nR + i] = r.byte();
  std::vector<double> ph(nP, -1);
  for (size_t i = 0; i < nP; ++i)
    if (fl[nR + i] & 8) ph[i] = double(r.uv()) / 2;
  std::vector<size_t> vc(nP);
  for (size_t i = 0; i < nP; ++i) vc[i] = size_t(r.uv());

  mvt::Tile tile;
  auto& layer = tile.layers.emplace_back();
  layer.name = "building";
  layer.features.reserve(nR + nP);
  auto add = [&](mvt::Ring&& ring, double h, int k) {
    auto& f = layer.features.emplace_back();
    f.type = mvt::GeomType::Polygon;
    f.parts.push_back(std::move(ring));
    f.props["h"] = h;
    f.props["k"] = double(k);
  };
  for (size_t i = 0; i < nR; ++i) {
    const double t = A[i] / AQ * kPi, c = std::cos(t), s = std::sin(t), l = L[i] / LQ / 2, w = W[i] / LQ / 2;
    const double ux = c * l, uy = s * l, vx = -s * w, vy = c * w, X = cx[i], Y = cy[i];
    add({pt(X + ux + vx, Y + uy + vy), pt(X + ux - vx, Y + uy - vy), pt(X - ux - vx, Y - uy - vy), pt(X - ux + vx, Y - uy + vy)},
        H[i], fl[i] & 3);
  }
  const double sx = Wm / PQ, sy = Hm / PQ;
  double fx = 0, fy = 0;
  for (size_t i = 0; i < nP; ++i) {
    fx += r.sv();
    fy += r.sv();
    double x = fx, y = fy, ar = 0;
    mvt::Ring ring;
    std::vector<double> xs{x}, ys{y};
    for (size_t j = 1; j < vc[i]; ++j) {
      x += r.sv();
      y += r.sv();
      xs.push_back(x);
      ys.push_back(y);
    }
    for (size_t j = 0; j < xs.size(); ++j) {
      const size_t k = (j + 1) % xs.size();
      ar += xs[j] * ys[k] - xs[k] * ys[j];
    }
    ring.reserve(xs.size());
    for (size_t j = 0; j < xs.size(); ++j) ring.push_back(pt(xs[j] * sx, ys[j] * sy));
    const double area = std::abs(ar) / 2 * sx * sy;
    const size_t b = nR + i;
    add(std::move(ring), ph[i] >= 0 ? ph[i] : estimate_height(area, fl[b] & 3, fl[b] & 4), fl[b] & 3);
  }
  return tile;
}

}  // namespace gsa::gsb
