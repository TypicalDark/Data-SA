#include "mvt.hpp"

#include <stdexcept>

#include <protozero/pbf_reader.hpp>

namespace gsa::mvt {

std::optional<double> Feature::num(const std::string& k) const {
  auto it = props.find(k);
  if (it == props.end()) return std::nullopt;
  if (auto d = std::get_if<double>(&it->second)) return *d;
  if (auto i = std::get_if<int64_t>(&it->second)) return double(*i);
  if (auto b = std::get_if<bool>(&it->second)) return *b ? 1.0 : 0.0;
  return std::nullopt;
}

std::string Feature::str(const std::string& k) const {
  auto it = props.find(k);
  if (it == props.end()) return {};
  if (auto s = std::get_if<std::string>(&it->second)) return *s;
  if (auto d = std::get_if<double>(&it->second)) return std::to_string(*d);
  if (auto i = std::get_if<int64_t>(&it->second)) return std::to_string(*i);
  return {};
}

const Layer* Tile::layer(const std::string& name) const {
  for (const auto& l : layers)
    if (l.name == name) return &l;
  return nullptr;
}

namespace {

Value read_value(protozero::pbf_reader v) {
  while (v.next()) {
    switch (v.tag()) {
      case 1: return v.get_string();
      case 2: return double(v.get_float());
      case 3: return v.get_double();
      case 4: return v.get_int64();
      case 5: return int64_t(v.get_uint64());
      case 6: return v.get_sint64();
      case 7: return v.get_bool();
      default: v.skip();
    }
  }
  return std::monostate{};
}

void read_geometry(Feature& f, const std::vector<uint32_t>& cmds, float inv_extent) {
  auto it = cmds.begin(), end = cmds.end();
  int32_t x = 0, y = 0;
  Ring cur;
  auto zz = [](uint32_t n) { return int32_t((n >> 1) ^ (~(n & 1) + 1)); };
  while (it != end) {
    const uint32_t cmd_int = *it++;
    const uint32_t cmd = cmd_int & 7, count = cmd_int >> 3;
    if (cmd == 1 || cmd == 2) {  // MoveTo, LineTo
      for (uint32_t i = 0; i < count; ++i) {
        if (it == end) throw std::runtime_error("truncated geometry");
        x += zz(*it++);
        if (it == end) throw std::runtime_error("truncated geometry");
        y += zz(*it++);
        if (cmd == 1 && f.type != GeomType::Point && !cur.empty()) {
          f.parts.push_back(std::move(cur));
          cur.clear();
        }
        cur.push_back({float(x) * inv_extent, float(y) * inv_extent});
      }
    } else if (cmd == 7) {  // ClosePath
      if (!cur.empty()) cur.push_back(cur.front());
    } else {
      throw std::runtime_error("bad geometry command");
    }
  }
  if (!cur.empty()) f.parts.push_back(std::move(cur));
}

Layer read_layer(protozero::pbf_reader l) {
  Layer layer;
  std::vector<std::string> keys;
  std::vector<Value> values;
  std::vector<protozero::data_view> feats;
  while (l.next()) {
    switch (l.tag()) {
      case 1: layer.name = l.get_string(); break;
      case 2: feats.push_back(l.get_view()); break;
      case 3: keys.push_back(l.get_string()); break;
      case 4: values.push_back(read_value(l.get_message())); break;
      case 5: layer.extent = l.get_uint32(); break;
      default: l.skip();
    }
  }
  if (layer.extent == 0) throw std::runtime_error("zero extent");
  const float inv = 1.0f / float(layer.extent);
  layer.features.reserve(feats.size());
  for (const auto& view : feats) {
    protozero::pbf_reader fr(view);
    Feature f;
    std::vector<uint32_t> tags, geom;
    while (fr.next()) {
      switch (fr.tag()) {
        case 1: f.id = fr.get_uint64(); break;
        case 2: {
          auto r = fr.get_packed_uint32();
          tags.assign(r.begin(), r.end());
          break;
        }
        case 3: f.type = static_cast<GeomType>(fr.get_enum()); break;
        case 4: {
          auto r = fr.get_packed_uint32();
          geom.assign(r.begin(), r.end());
          break;
        }
        default: fr.skip();
      }
    }
    for (size_t i = 0; i + 1 < tags.size(); i += 2) {
      if (tags[i] < keys.size() && tags[i + 1] < values.size()) f.props[keys[tags[i]]] = values[tags[i + 1]];
    }
    read_geometry(f, geom, inv);
    layer.features.push_back(std::move(f));
  }
  return layer;
}

}  // namespace

Tile decode(const std::string& data) {
  Tile t;
  protozero::pbf_reader r(data);
  while (r.next(3)) t.layers.push_back(read_layer(r.get_message()));
  return t;
}

double signed_area(const Ring& r) {
  double a = 0;
  for (size_t i = 0, j = r.size() ? r.size() - 1 : 0; i < r.size(); j = i++) {
    a += double(r[j].x) * r[i].y - double(r[i].x) * r[j].y;
  }
  return a / 2;
}

std::vector<std::vector<size_t>> group_polygons(const Feature& f) {
  std::vector<std::vector<size_t>> out;
  for (size_t i = 0; i < f.parts.size(); ++i) {
    const double a = signed_area(f.parts[i]);
    if (a == 0) continue;
    // MVT 2.x: exterior rings are clockwise in screen coordinates, i.e. positive area here
    if (a > 0 || out.empty()) out.push_back({i});
    else out.back().push_back(i);
  }
  return out;
}

bool point_in_polygon(const Feature& f, float x, float y) {
  bool inside = false;  // even-odd over all rings handles holes
  for (const auto& r : f.parts) {
    for (size_t i = 0, j = r.size() ? r.size() - 1 : 0; i < r.size(); j = i++) {
      if (((r[i].y > y) != (r[j].y > y)) && (x < (r[j].x - r[i].x) * (y - r[i].y) / (r[j].y - r[i].y) + r[i].x)) {
        inside = !inside;
      }
    }
  }
  return inside;
}

}  // namespace gsa::mvt
