// Starts the real back-end the way the app does (random port, one-time token), fetches tiles
// through the API client, decodes them and builds meshes.
#include <doctest/doctest.h>

#include "api_client.hpp"
#include "backend_process.hpp"
#include "layers.hpp"
#include "mesh.hpp"
#include "mvt.hpp"

using namespace gsa;

TEST_CASE("polygon helpers") {
  mvt::Feature sq;
  sq.type = mvt::GeomType::Polygon;
  sq.parts = {{{0.1f, 0.1f}, {0.9f, 0.1f}, {0.9f, 0.9f}, {0.1f, 0.9f}, {0.1f, 0.1f}}};
  CHECK(mvt::signed_area(sq.parts[0]) == doctest::Approx(0.64));
  CHECK(mvt::point_in_polygon(sq, 0.5f, 0.5f));
  CHECK_FALSE(mvt::point_in_polygon(sq, 0.95f, 0.5f));
  std::vector<mesh::Vertex> v;
  mesh::add_polygon(sq, 0xffffffffu, v);
  CHECK(v.size() == 6);  // two triangles
  std::vector<mesh::Vertex> l;
  mesh::add_line(sq.parts[0], 1.0f, 0xffffffffu, 0, false, l);
  CHECK(l.size() == 4 * 6);
}

#ifdef GSA_BACKEND_EXE
TEST_CASE("back-end process, API client, MVT decode and meshing end to end") {
  BackendProcess be(GSA_BACKEND_EXE, GSA_DEVDATA);
  REQUIRE(be.port() > 0);
  CHECK(be.token().size() == 64);

  api::Client anon({"127.0.0.1", be.port(), ""});
  CHECK(anon.get("/v1/health").ok());
  CHECK(anon.get("/v1/version").status == 401);

  api::Client c({"127.0.0.1", be.port(), be.token()});
  auto v = c.get_json("/v1/version");
  REQUIRE(v.has_value());
  CHECK((*v)["api"] == "1");

  auto r = c.tile("admin", 5, 17, 19);  // Cape Town
  REQUIRE(r.status == 200);
  const auto tile = mvt::decode(r.body);
  const auto* munis = tile.layer("municipality");
  REQUIRE(munis != nullptr);
  CHECK(munis->features.size() > 10);
  bool cpt = false;
  for (const auto& f : munis->features) cpt |= f.str("name") == "City of Cape Town";
  CHECK(cpt);
  REQUIRE(tile.layer("province") != nullptr);

  mesh::Mesh m;
  layers::MapStyle people;
  people.fill = style::Topic::People;
  mesh::build("admin", tile, people, m);
  CHECK(m.fill.size() > 1000);
  CHECK(m.fill.size() % 3 == 0);
  CHECK(m.lines.size() > 1000);
  std::vector<mesh::Label> labels;
  mesh::labels(tile, 17 / 32.0, 19 / 32.0, 1 / 32.0, labels);
  CHECK(!labels.empty());

  auto g = c.tile("grid", 5, 18, 18);
  REQUIRE(g.status == 200);
  mesh::Mesh gm;
  layers::MapStyle grid;
  grid.lines = true;
  mesh::build("grid", mvt::decode(g.body), grid, gm);
  CHECK(gm.lines.size() > 100);
  mesh::Mesh none;
  mesh::build("grid", mvt::decode(g.body), people, none);
  CHECK(none.vertex_count() == 0);  // the grid is only drawn in its own topic

  const auto hits = c.search("cape town", 5);
  REQUIRE(!hits.empty());
  CHECK(hits[0].label == "City of Cape Town");
  CHECK(c.tile("admin", 9, 0, 0).status == 204);
}
#endif
