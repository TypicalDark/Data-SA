// Plot card, time series and grid trace routes over a small pack built here (gsa-pack/1 tables).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "gsa/api.hpp"
#include "gsa/graph.hpp"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Grid used below (kV in brackets, km on the lines):
//
//   A[400] --e1 400kV 10km-- t2 --e2 132kV 5km-- B[132] --e3 132kV 4km-- C[132] --e4 275kV 20km-- E[275]
//                                                  |
//                                              e5 66kV 3km
//                                                  |
//                                                F[66]
void make_pack(const fs::path& file) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(file.string().c_str(), &db) == SQLITE_OK);
  const char* sql = R"SQL(
    CREATE TABLE pack_info (k TEXT PRIMARY KEY, v TEXT);
    INSERT INTO pack_info VALUES ('format','gsa-pack/1');
    CREATE TABLE tree (area_id TEXT PRIMARY KEY, level INT, parent_id TEXT, path TEXT, name TEXT, minx REAL, miny REAL,
                       maxx REAL, maxy REAL, lon REAL, lat REAL, area_km2 REAL);
    INSERT INTO tree VALUES ('ZA',0,NULL,'ZA','South Africa',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
                            ('WC',1,'ZA','ZA.WC','Western Cape',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
                            ('WC053',3,'WC','ZA.WC.WC053','Beaufort West',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
                            ('W10503001',4,'WC053','ZA.WC.WC053.W10503001','Ward 1',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
                            ('SX1',6,'W10503001','ZA.WC.WC053.W10503001.SX1','Hospital Hill',NULL,NULL,NULL,NULL,NULL,NULL,NULL);
    CREATE TABLE metric (area_id TEXT, metric TEXT, as_of TEXT, v REAL, n INT, source_id TEXT, PRIMARY KEY (area_id, metric, as_of));
    CREATE TABLE source (source_id TEXT PRIMARY KEY, title TEXT, publisher TEXT, url TEXT, licence TEXT, resale_ok INT, cadence TEXT, privacy TEXT);
    CREATE VIRTUAL TABLE search USING fts5(ref UNINDEXED, label, detail, weight UNINDEXED, lon UNINDEXED, lat UNINDEXED);
    INSERT INTO search VALUES ('area:WC053','Beaufort West','Municipality, Western Cape',7,22.58,-32.35);
    CREATE TABLE parcel (sg21 TEXT PRIMARY KEY, ward_id TEXT, suburb_id TEXT, kind TEXT, town TEXT, parcel_no TEXT, portion_no TEXT,
                         area_m2 REAL, h3_r9 TEXT, quadkey_z16 TEXT, lon REAL, lat REAL);
    INSERT INTO parcel VALUES ('C05200010000002300000','W10503001','SX1','erf','BEAUFORT WEST','23','0',1190,'89...','3001',22.58,-32.35);
    CREATE TABLE valuation (sg21 TEXT, muni_code TEXT, roll_period TEXT, category TEXT, usage TEXT, extent_m2 REAL,
                            market_value INT, sale_price INT, sale_date TEXT, address TEXT, source_url TEXT);
    INSERT INTO valuation VALUES ('C05200010000002300000','WC053','2023-2027','RESIDENTIAL','1214',1190,760000,690000,'2021-03-04',
                                  '12 DONKIN ST','https://example.org/roll.pdf');
    CREATE TABLE series (series_id INT PRIMARY KEY, entity_type TEXT, entity_id TEXT, variable TEXT, unit TEXT, step TEXT, source_id TEXT);
    INSERT INTO series VALUES (1,'area','W10503001','rain_mm','mm','month','nasa_power_monthly');
    CREATE TABLE series_data (series_id INT PRIMARY KEY, t0 TEXT, step TEXT, n INT, v BLOB);
    CREATE TABLE node (network TEXT, node_id INT, ext_id TEXT, kind TEXT, name TEXT, operator TEXT, kv TEXT, capacity_mva REAL,
                       lon REAL, lat REAL, PRIMARY KEY (network, node_id));
    INSERT INTO node VALUES ('power',1,'sub:a','substation','A','Eskom','400/132',NULL,22,-32),
                            ('power',2,'n2','endpoint',NULL,NULL,'',NULL,22.1,-32),
                            ('power',3,'sub:b','substation','B','Eskom','132',NULL,22.2,-32),
                            ('power',4,'sub:c','substation','C','Eskom','132',NULL,22.3,-32),
                            ('power',6,'sub:e','substation','E','Eskom','275/132',NULL,22.5,-32),
                            ('power',7,'sub:f','substation','F','Muni','66',NULL,22.2,-32.1);
    CREATE TABLE edge (network TEXT, edge_id INT, source INT, target INT, cost REAL, reverse_cost REAL, cls TEXT, kv TEXT,
                       inferred INT, reason TEXT, PRIMARY KEY (network, edge_id));
    INSERT INTO edge VALUES ('power',1,1,2,10,10,'transmission','400',0,'osm'),
                            ('power',2,2,3,5,5,'subtransmission','132',0,'osm'),
                            ('power',3,3,4,4,4,'subtransmission','132',1,'gap join'),
                            ('power',4,4,6,20,20,'transmission','275',0,'osm'),
                            ('power',5,3,7,3,3,'distribution','66',0,'osm');
  )SQL";
  char* err = nullptr;
  REQUIRE_MESSAGE(sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK, (err ? err : ""));
  // rain: Jan 10.5, Feb missing, Mar 30
  const float v[3] = {10.5f, std::nanf(""), 30.0f};
  sqlite3_stmt* st = nullptr;
  sqlite3_prepare_v2(db, "INSERT INTO series_data VALUES (1,'2025-01-01','month',3,?)", -1, &st, nullptr);
  sqlite3_bind_blob(st, 1, v, sizeof v, SQLITE_TRANSIENT);
  REQUIRE(sqlite3_step(st) == SQLITE_DONE);
  sqlite3_finalize(st);
  sqlite3_close(db);
}

// A province pack (pack-wc.sqlite) as export_pack.py writes it: plots, their values and plot search only.
// Newer columns: parcel.stand / superseded_by, valuation.roll / zoning / value_flag.
void make_province_pack(const fs::path& file) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(file.string().c_str(), &db) == SQLITE_OK);
  const char* sql = R"SQL(
    CREATE TABLE pack_info (k TEXT PRIMARY KEY, v TEXT);
    INSERT INTO pack_info VALUES ('format','gsa-pack/1'), ('region','WC');
    CREATE TABLE parcel (sg21 TEXT PRIMARY KEY, ward_id TEXT, suburb_id TEXT, kind TEXT, town TEXT, parcel_no TEXT, portion_no TEXT,
                         area_m2 REAL, h3_r9 TEXT, quadkey_z16 TEXT, lon REAL, lat REAL, stand TEXT, superseded_by TEXT);
    INSERT INTO parcel VALUES ('C05200010000002400000','W10503001',NULL,'erf','BEAUFORT WEST','24','0',900,NULL,NULL,22.59,-32.35,
                               'RE/24',NULL);
    CREATE TABLE valuation (sg21 TEXT, muni_code TEXT, roll_period TEXT, category TEXT, usage TEXT, extent_m2 REAL,
                            market_value INT, sale_price INT, sale_date TEXT, address TEXT, source_url TEXT,
                            roll TEXT, zoning TEXT, value_flag TEXT);
    INSERT INTO valuation VALUES ('C05200010000002400000','WC053','2023-2027','RES',NULL,900,500000,NULL,NULL,'14 DONKIN ST',
                                  'https://example.org/roll.pdf','general','Residential 1',NULL),
                                 ('C05200010000002400000','WC053',NULL,'RES',NULL,900,480000,NULL,NULL,NULL,
                                  'https://example.org/layer','other layer',NULL,'superseded');
    CREATE VIRTUAL TABLE search USING fts5(ref UNINDEXED, label, detail, weight UNINDEXED, lon UNINDEXED, lat UNINDEXED);
    INSERT INTO search VALUES ('parcel:C05200010000002400000','RE/24 BEAUFORT WEST','erf',1,22.59,-32.35);
  )SQL";
  char* err = nullptr;
  REQUIRE_MESSAGE(sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK, (err ? err : ""));
  sqlite3_close(db);
}

struct DataServer {
  fs::path dir;
  httplib::Server srv;
  int port = 0;
  std::thread th;
  DataServer() {
    dir = fs::temp_directory_path() / ("gsa_data_api_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir / "packs");
    make_pack(dir / "packs" / "pack.sqlite");
    make_province_pack(dir / "packs" / "pack-wc.sqlite");
    auto store = std::make_shared<gsa::PackStore>(dir);
    gsa::mount_api(srv, gsa::ApiConfig{store, "", ""});
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    srv.wait_until_ready();
  }
  ~DataServer() {
    srv.stop();
    th.join();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};
DataServer& ds() {
  static DataServer s;
  return s;
}
json get(const std::string& path, int want = 200) {
  httplib::Client c("127.0.0.1", ds().port);
  auto r = c.Get(path);
  REQUIRE(r);
  CHECK_MESSAGE(r->status == want, path << " -> " << r->status << " " << r->body);
  return json::parse(r->body);
}
std::vector<int64_t> ids(const json& a) { return a.get<std::vector<int64_t>>(); }

}  // namespace

TEST_CASE("max_kv reads voltage lists") {
  CHECK(gsa::graph::max_kv("400/132") == 400);
  CHECK(gsa::graph::max_kv("") == 0);
  CHECK(gsa::graph::max_kv("66;11") == 66);
}

TEST_CASE("plot card") {
  const auto p = get("/v1/parcels/C05200010000002300000");
  CHECK(p["kind"] == "PLOT");
  CHECK(p["market_value"] == 760000);
  CHECK(p["valuations"].size() == 1);
  CHECK(p["valuations"][0]["source_url"] == "https://example.org/roll.pdf");
  CHECK(p["sg"]["township"] == "C0520001");
  REQUIRE(p["breadcrumb"].size() == 5);
  CHECK(p["breadcrumb"][0]["id"] == "ZA");
  CHECK(p["breadcrumb"][3]["id"] == "W10503001");
  CHECK(p["breadcrumb"][4]["name"] == "Hospital Hill");
  get("/v1/parcels/C99999999999999999999", 404);
  // not an SG code: no route
  httplib::Client c("127.0.0.1", ds().port);
  CHECK(c.Get("/v1/parcels/abc")->status == 404);
}

TEST_CASE("time series") {
  const auto list = get("/v1/series?entity=W10503001");
  REQUIRE(list.size() == 1);
  CHECK(list[0]["variable"] == "rain_mm");
  const auto s = get("/v1/series?entity=W10503001&variable=rain_mm");
  CHECK(s["t0"] == "2025-01-01");
  CHECK(s["step"] == "month");
  REQUIRE(s["values"].size() == 3);
  CHECK(s["values"][0] == 10.5);
  CHECK(s["values"][1].is_null());
  CHECK(s["values"][2] == 30.0);
  get("/v1/series?entity=W10503001&variable=nope", 404);
  get("/v1/series", 400);
}

TEST_CASE("graph node") {
  const auto n = get("/v1/graph/power/nodes/3");
  CHECK(n["name"] == "B");
  CHECK(n["edges"].size() == 3);
  get("/v1/graph/power/nodes/99", 404);
}

TEST_CASE("trace: connected, path, feed, fail") {
  auto t = get("/v1/trace?from=3");
  auto nodes = ids(t["nodes"]);
  std::sort(nodes.begin(), nodes.end());
  CHECK(nodes == std::vector<int64_t>{1, 2, 3, 4, 6, 7});
  CHECK(t["km"] == 42);

  t = get("/v1/trace?from=3&min_kv=132");  // the 66 kV spur drops out
  CHECK(t["nodes"].size() == 5);

  t = get("/v1/trace?mode=path&from=1&to=4");
  CHECK(t["found"] == true);
  CHECK(ids(t["edges"]) == std::vector<int64_t>{1, 2, 3});
  CHECK(t["km"] == 19);

  t = get("/v1/trace?mode=feed&from=7");  // nearest 220 kV+ sources: A (18 km), then E (27 km)
  REQUIRE(t["sources"].size() == 2);
  CHECK(t["sources"][0]["name"] == "A");
  CHECK(t["sources"][0]["km"] == 18);
  CHECK(ids(t["sources"][0]["path"]["edges"]) == std::vector<int64_t>{5, 2, 1});
  CHECK(t["sources"][1]["name"] == "E");

  t = get("/v1/trace?mode=fail&from=3");  // losing B strands F only; C is still fed from E
  CHECK(ids(t["affected"]["nodes"]) == std::vector<int64_t>{7});
  CHECK(ids(t["affected"]["edges"]) == std::vector<int64_t>{5});

  t = get("/v1/trace?mode=fail&edge=4");  // the 275 kV line: everything still reaches A
  CHECK(t["affected"]["nodes"].empty());

  get("/v1/trace?mode=path&from=1", 400);
  get("/v1/trace?mode=nope&from=1", 400);
  get("/v1/trace?from=abc", 400);
  get("/v1/trace?from=99", 404);
}

TEST_CASE("plots in a province pack") {
  const auto p = get("/v1/parcels/C05200010000002400000");
  CHECK(p["stand"] == "RE/24");
  CHECK(!p.contains("superseded_by"));
  REQUIRE(p["valuations"].size() == 2);
  // the superseded entry is listed with its flag but not counted
  CHECK(p["market_value"] == 500000);
  CHECK(p["valuations"][0]["zoning"] == "Residential 1");
  CHECK(p["valuations"][1]["flag"] == "superseded");
  REQUIRE(p["breadcrumb"].size() == 4);  // tree from the national pack
  CHECK(p["breadcrumb"][3]["id"] == "W10503001");
}

TEST_CASE("search merges the national and province packs") {
  const auto h = get("/v1/search?q=beaufort");
  REQUIRE(h.size() == 2);
  CHECK(h[0]["ref"] == "area:WC053");  // exact label first, and the municipality outranks the plot
  CHECK(h[1]["ref"] == "parcel:C05200010000002400000");
  CHECK(get("/v1/search?q=beaufort&limit=1").size() == 1);
}
