#include <doctest/doctest.h>

#include <cmath>
#include <set>

#include "gsa/gzip.hpp"
#include "gsa/pmtiles.hpp"

using namespace gsa::pmtiles;

TEST_CASE("tile ids follow the PMTiles v3 spec") {
  CHECK(zxy_to_tile_id(0, 0, 0) == 0u);
  CHECK(zxy_to_tile_id(1, 0, 0) == 1u);
  CHECK(zxy_to_tile_id(1, 0, 1) == 2u);
  CHECK(zxy_to_tile_id(1, 1, 1) == 3u);
  CHECK(zxy_to_tile_id(1, 1, 0) == 4u);
  CHECK(zxy_to_tile_id(2, 0, 0) == 5u);
  CHECK_FALSE(zxy_to_tile_id(1, 2, 0).has_value());
  CHECK_FALSE(zxy_to_tile_id(40, 0, 0).has_value());
}

TEST_CASE("tile ids are a bijection within each zoom") {
  for (uint8_t z = 0; z < 7; ++z) {
    const uint64_t n = 1u << z, base = ((uint64_t(1) << (2 * z)) - 1) / 3;
    std::set<uint64_t> seen;
    for (uint32_t x = 0; x < n; ++x)
      for (uint32_t y = 0; y < n; ++y) {
        const auto id = *zxy_to_tile_id(z, x, y);
        CHECK(id >= base);
        CHECK(id < base + n * n);
        seen.insert(id);
      }
    CHECK(seen.size() == n * n);
  }
}

TEST_CASE("directories decode, including contiguous offsets") {
  // 2 entries: ids 0 and 1, run 1 each, lengths 10 and 5, offsets 0 (stored 1) and contiguous (stored 0)
  std::vector<uint8_t> b{2, 0, 1, 1, 1, 10, 5, 1, 0};
  const auto d = decode_directory(b);
  REQUIRE(d.size() == 2);
  CHECK(d[0] == Entry{0, 0, 10, 1});
  CHECK(d[1] == Entry{1, 10, 5, 1});
  b.resize(3);
  CHECK_THROWS_AS(decode_directory(b), Error);
}

TEST_CASE("find_entry honours run lengths and leaf pointers") {
  const std::vector<Entry> d{{5, 0, 1, 3}, {20, 1, 1, 0}};
  CHECK_FALSE(find_entry(d, 4).has_value());
  CHECK(find_entry(d, 7)->tile_id == 5u);
  CHECK_FALSE(find_entry(d, 8).has_value());
  CHECK(find_entry(d, 25)->run_length == 0u);  // falls into the leaf after 20
}

TEST_CASE("header rejects other files") {
  std::vector<uint8_t> b(127, 0);
  CHECK_THROWS_AS(Header::parse(b.data(), b.size()), Error);
}

namespace {
// lon/lat to tile at zoom z (web mercator)
std::pair<uint32_t, uint32_t> lonlat_tile(double lon, double lat, int z) {
  constexpr double pi = 3.14159265358979323846;
  const double n = std::pow(2.0, z), r = lat * pi / 180.0;
  return {uint32_t((lon + 180.0) / 360.0 * n), uint32_t((1.0 - std::asinh(std::tan(r)) / pi) / 2.0 * n)};
}
}  // namespace

TEST_CASE("reads real tiles from the development admin pack") {
  const Archive a(std::string(GSA_DEVDATA) + "/packs/admin.pmtiles");
  const auto& h = a.header();
  CHECK(h.tile_type == TileType::Mvt);
  CHECK(h.min_zoom == 0);
  CHECK(h.max_zoom == 10);
  CHECK(h.bounds[0] > 15.0);
  CHECK(h.bounds[2] < 34.0);
  CHECK(a.metadata().contains("vector_layers"));

  // Cape Town at z5, z8 and z10 (z10 goes through leaf directories in larger packs)
  for (int z : {0, 5, 8, 10}) {
    const auto [x, y] = lonlat_tile(18.47, -33.92, z);
    const auto t = a.tile(uint8_t(z), x, y);
    REQUIRE_MESSAGE(t.has_value(), "z" << z);
    auto raw = t->compression == Compression::Gzip ? gsa::gunzip(t->data.data(), t->data.size()) : t->data;
    REQUIRE(!raw.empty());
    CHECK(raw[0] == 0x1a);  // MVT: field 3 (layer), wire type 2
  }
  // the middle of the Atlantic has no tile; outside the zoom range neither
  const auto [x, y] = lonlat_tile(-30.0, -40.0, 8);
  CHECK_FALSE(a.tile(8, x, y).has_value());
  CHECK_FALSE(a.tile(12, 0, 0).has_value());
}

TEST_CASE("gunzip guards size and corruption") {
  const uint8_t junk[] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3, 1, 2, 3};
  CHECK_THROWS(gsa::gunzip(junk, sizeof junk));
}
