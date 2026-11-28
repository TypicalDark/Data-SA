#include <doctest/doctest.h>

#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gsa/api.hpp"

using nlohmann::json;

namespace {

// One back-end on a random port for the whole test binary, serving devdata/.
struct Server {
  httplib::Server srv;
  int port = 0;
  std::thread th;
  Server() {
    auto store = std::make_shared<gsa::PackStore>(GSA_DEVDATA);
    gsa::mount_api(srv, gsa::ApiConfig{store, "test-token", ""});
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    srv.wait_until_ready();
  }
  ~Server() {
    srv.stop();
    th.join();
  }
};
Server& server() {
  static Server s;
  return s;
}
httplib::Client client(bool auth = true) {
  httplib::Client c("127.0.0.1", server().port);
  if (auth) c.set_bearer_token_auth("test-token");
  c.set_decompress(false);
  return c;
}
json get_json(const std::string& path, int want = 200) {
  auto c = client();
  auto r = c.Get(path);
  REQUIRE(r);
  CHECK_MESSAGE(r->status == want, path << " -> " << r->status << " " << r->body);
  return json::parse(r->body);
}

}  // namespace

TEST_CASE("fts_query makes safe prefix terms") {
  CHECK(gsa::fts_query("Beaufort West") == "\"beaufort\"* \"west\"*");
  CHECK(gsa::fts_query("  \"x\" OR y* NEAR(") == "\"x\"* \"or\"* \"y\"* \"near\"*");
  CHECK(gsa::fts_query("()*:^-") == "");
  CHECK(gsa::fts_query("Ñandú") == "\"Ñandú\"*");  // UTF-8 passes through for unicode61 to fold
}

TEST_CASE("kind tags") {
  CHECK(gsa::kind_for("area:WC053", 3, "") == "MUNI");
  CHECK(gsa::kind_for("area:WC", 1, "") == "PROV");
  CHECK(gsa::kind_for("node:power:1", std::nullopt, "substation 132 kV Eskom") == "SUB");
  CHECK(gsa::kind_for("node:power:2", std::nullopt, "power station coal 4100 MW") == "PLANT");
  CHECK(gsa::kind_for("parcel:C0290002000000040000", std::nullopt, "") == "PLOT");
}

TEST_CASE("token comparison") {
  CHECK(gsa::token_equal("abc", "abc"));
  CHECK_FALSE(gsa::token_equal("abc", "abd"));
  CHECK_FALSE(gsa::token_equal("abc", "abcd"));
  CHECK_FALSE(gsa::token_equal("", "a"));
}

TEST_CASE("health needs no token, everything else does") {
  auto anon = client(false);
  auto h = anon.Get("/v1/health");
  REQUIRE(h);
  CHECK(h->status == 200);
  CHECK(json::parse(h->body)["status"] == "ok");
  for (const char* p : {"/v1/version", "/v1/packs", "/v1/search?q=cape", "/v1/areas/ZA", "/v1/tiles/admin/0/0/0"}) {
    auto r = anon.Get(p);
    REQUIRE(r);
    CHECK_MESSAGE(r->status == 401, p);
  }
  httplib::Client wrong("127.0.0.1", server().port);
  wrong.set_bearer_token_auth("nope");
  CHECK(wrong.Get("/v1/version")->status == 401);
}

TEST_CASE("version and packs") {
  const auto v = get_json("/v1/version");
  CHECK(v["api"] == "1");
  CHECK(v["packs"]["admin"] == "dev");
  CHECK(v["packs"]["pack"] == "dev");
  const auto packs = get_json("/v1/packs");
  bool admin = false;
  for (const auto& p : packs) {
    if (p["id"] != "admin") continue;
    admin = true;
    CHECK(p["kind"] == "tiles");
    CHECK(p["tile_type"] == "mvt");
    std::set<std::string> layers;
    for (const auto& l : p["layers"]) layers.insert(l["id"]);
    CHECK(layers.count("municipality") == 1);
    CHECK(layers.count("province") == 1);
  }
  CHECK(admin);
}

TEST_CASE("data packs: the Find data") {
  const auto f = get_json("/v1/data/find");
  CHECK(f["munis"].size() > 200);
  CHECK(f["cap"].size() > 100);
  get_json("/v1/data/nothing", 404);
  bool listed = false;
  for (const auto& p : get_json("/v1/packs"))
    if (p["id"] == "find") listed = p["kind"] == "json";
  CHECK(listed);
}

TEST_CASE("tiles: bytes, encoding, empty and errors") {
  auto c = client();
  auto r = c.Get("/v1/tiles/admin/5/17/18");  // Western Cape
  REQUIRE(r);
  CHECK(r->status == 200);
  CHECK(r->get_header_value("Content-Type") == "application/vnd.mapbox-vector-tile");
  CHECK(r->get_header_value("Content-Encoding") == "gzip");
  CHECK(r->body.size() > 100);
  CHECK(c.Get("/v1/tiles/admin/5/17/18.mvt")->status == 200);
  CHECK(c.Get("/v1/tiles/admin/8/0/0")->status == 204);          // no land there
  CHECK(c.Get("/v1/tiles/admin/5/40/0")->status == 400);         // x beyond 2^z
  CHECK(c.Get("/v1/tiles/nosuch/0/0/0")->status == 404);
  CHECK(c.Get("/v1/tiles/../packs/0/0/0")->status == 404);       // no route for odd ids
  const auto tj = get_json("/v1/tilejson/grid");
  CHECK(tj["tiles"][0] == "/v1/tiles/grid/{z}/{x}/{y}");
}

TEST_CASE("search ranks municipalities above their namesakes") {
  const auto hits = get_json("/v1/search?q=beaufort");
  REQUIRE(!hits.empty());
  CHECK(hits[0]["label"] == "Beaufort West");
  CHECK(hits[0]["kind"] == "MUNI");
  CHECK(hits[0]["ref"] == "area:WC053");
  const auto cape = get_json("/v1/search?q=western%20cape&limit=3");
  REQUIRE(!cape.empty());
  CHECK(cape[0]["kind"] == "PROV");
  CHECK(cape.size() <= 3);
  CHECK(get_json("/v1/search?q=%22%28%29", 200).empty());
  get_json("/v1/search?q=", 400);
}

TEST_CASE("area tree: breadcrumb, children, metrics") {
  const auto za = get_json("/v1/areas/ZA");
  CHECK(za["kind"] == "COUNTRY");
  CHECK(za["children"].size() == 9);
  CHECK(za["breadcrumb"].empty());
  const auto m = get_json("/v1/areas/WC053");
  CHECK(m["name"] == "Beaufort West");
  CHECK(m["kind"] == "MUNI");
  REQUIRE(m["breadcrumb"].size() == 2);
  CHECK(m["breadcrumb"][0]["id"] == "ZA");
  CHECK(m["breadcrumb"][1]["id"] == "WC");
  bool pop = false;
  for (const auto& x : m["metrics"]) pop |= x["metric"] == "pop_est" && x["v"].get<double>() > 1000;
  CHECK(pop);
  const auto wc = get_json("/v1/areas/WC");
  CHECK(wc["children"].size() > 20);
  CHECK(wc["children"][0]["metrics"].contains("pop_est"));
  get_json("/v1/areas/NOPE", 404);
}

TEST_CASE("sources carry licences and the resale flag") {
  const auto s = get_json("/v1/sources");
  bool rolls = false;
  for (const auto& x : s)
    if (x["source_id"] == "valuation_rolls") rolls = x["resale_ok"] == false;
  CHECK(rolls);
}
