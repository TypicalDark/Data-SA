// Live planes and ships (store, parsers, poller, routes) and the caching layer (ETag, 304, gzip, response cache).
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gsa/api.hpp"
#include "gsa/gzip.hpp"
#include "gsa/live.hpp"

using nlohmann::json;
using gsa::live::Fix;

namespace {

Fix fix(const std::string& id, double t, double lon, double lat, const std::string& name = "") {
  Fix f;
  f.id = id, f.t = t, f.lon = lon, f.lat = lat, f.name = name;
  f.spd = 400, f.hdg = 90, f.alt = 35000;
  return f;
}

struct LiveServer {
  httplib::Server srv;
  int port = 0;
  std::thread th;
  std::shared_ptr<gsa::live::Store> live = std::make_shared<gsa::live::Store>(gsa::live::Options{});
  LiveServer() {
    gsa::ApiConfig cfg{std::make_shared<gsa::PackStore>(GSA_DEVDATA), "", ""};
    cfg.live = live;
    gsa::mount_api(srv, cfg);
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    srv.wait_until_ready();
  }
  ~LiveServer() {
    srv.stop();
    th.join();
  }
};
LiveServer& ls() {
  static LiveServer s;
  return s;
}
httplib::Client cl() {
  httplib::Client c("127.0.0.1", ls().port);
  c.set_decompress(false);
  return c;
}

}  // namespace

TEST_CASE("live store: latest, thinning, track, watch, prune") {
  gsa::live::Options o;
  o.min_gap_s = 60;
  o.min_move_m = 50;
  o.keep_s = 3600;
  gsa::live::Store s(o);
  const double t0 = 1.8e9;
  // a plane moving east, a fix every 10 s for 5 minutes (about 2 km per fix)
  std::vector<Fix> batch;
  size_t hist = 0;
  for (int i = 0; i <= 30; ++i) hist += s.ingest("plane", {fix("abc123", t0 + i * 10, 28.0 + i * 0.02, -26.0, "SAA301")}, "test", t0 + i * 10);
  CHECK(hist == 31);  // moving: every fix is kept (at least 5 s apart)
  // parked: one history point per minute
  size_t parked = 0;
  for (int i = 1; i <= 30; ++i) parked += s.ingest("plane", {fix("abc123", t0 + 300 + i * 10, 28.6, -26.0)}, "test", t0 + 300 + i * 10);
  CHECK(parked == 5);
  // an older fix is ignored
  CHECK(s.ingest("plane", {fix("abc123", t0, 0, 0)}, "test", t0 + 700) == 0);

  auto l = s.latest("plane", std::nullopt, 300, t0 + 600);
  REQUIRE(l["items"].size() == 1);
  CHECK(l["items"][0]["name"] == "SAA301");  // name kept from earlier fixes
  CHECK(l["items"][0]["lon"] == doctest::Approx(28.6));
  CHECK(s.latest("plane", gsa::live::Bbox{10, -40, 20, -30}, 300, t0 + 600)["n"] == 0);
  CHECK(s.latest("plane", std::nullopt, 10, t0 + 2000)["n"] == 0);  // too old

  auto tr = s.track("plane", "abc123", t0, t0 + 1000, 1000);
  CHECK(tr["n"] == 36);
  CHECK(tr["fixes"][0][0] == int64_t(t0));
  CHECK(tr["fixes"][0][1] == doctest::Approx(28.0));
  CHECK(tr["name"] == "SAA301");
  CHECK(s.track("plane", "abc123", t0, t0 + 1000, 5)["n"] == 5);  // the latest five

  // watched objects keep their history past keep_s; others are pruned
  s.ingest("ship", {fix("601000001", t0, 18.4, -33.9, "SA AGULHAS II")}, "test", t0);
  s.ingest("ship", {fix("601000002", t0, 18.5, -33.9)}, "test", t0);
  CHECK(s.watch("ship", "601000001", "Agulhas", t0));
  CHECK_FALSE(s.watch("ship", "601000001", "Agulhas", t0));
  CHECK(s.watched("ship", "601000001"));
  const size_t gone = s.prune(t0 + 7200);
  CHECK(gone == 36 + 1);  // the plane's history and the unwatched ship
  CHECK(s.track("ship", "601000001", 0, 1e11, 100)["n"] == 1);
  CHECK(s.track("ship", "601000002", 0, 1e11, 100)["n"] == 0);
  CHECK(s.watch_list().size() == 1);
  CHECK(s.unwatch("ship", "601000001"));
  CHECK_FALSE(s.unwatch("ship", "601000001"));
  CHECK_THROWS(s.ingest("train", {}, "x", t0));
}

TEST_CASE("plane feed parsers") {
  const auto a = gsa::live::parse_adsb_lol(R"({"now":1800000000000,"ac":[
    {"hex":"00a1b2","flight":"SAA301  ","r":"ZS-SXA","t":"A333","lat":-26.1,"lon":28.2,"alt_baro":35000,"gs":450.5,"track":201.3,"seen_pos":2.5},
    {"hex":"00a1b3","lat":-26.13,"lon":28.24,"alt_baro":"ground","gs":3},
    {"hex":"00a1b4","alt_baro":1000}]})");
  REQUIRE(a.size() == 2);
  CHECK(a[0].name == "SAA301");
  CHECK(a[0].t == doctest::Approx(1799999997.5));
  CHECK(*a[0].alt == 35000);
  CHECK(a[0].extra["reg"] == "ZS-SXA");
  CHECK(a[1].extra["gnd"] == true);
  CHECK(*a[1].alt == 0);

  const auto o = gsa::live::parse_opensky(R"({"time":1800000000,"states":[
    ["00a1b2","SAA301  ","South Africa",1799999990,1799999995,28.2,-26.1,10668,false,231.5,201.3,-2.5,null,10900,"4012",false,0],
    ["00a1b9","","South Africa",null,1799999995,null,null,null,true,0,0,null,null,null,null,false,0]]})");
  REQUIRE(o.size() == 1);
  CHECK(o[0].name == "SAA301");
  CHECK(*o[0].alt == doctest::Approx(35000).epsilon(0.01));  // 10668 m
  CHECK(*o[0].spd == doctest::Approx(450).epsilon(0.01));    // 231.5 m/s
  CHECK(o[0].extra["ctry"] == "South Africa");
}

TEST_CASE("plane poller falls back to OpenSky") {
  auto store = std::make_shared<gsa::live::Store>(gsa::live::Options{});
  const double now = gsa::live::now_s();
  std::vector<std::string> asked;
  gsa::live::PlanePoller p(store, [&](const std::string& url) -> std::optional<std::string> {
    asked.push_back(url);
    if (url.find("adsb.lol") != std::string::npos) return std::nullopt;  // down
    return json{{"time", now}, {"states", json::array({json::array({"00a1b2", "SAA301", "South Africa", now, now, 28.2, -26.1,
                                                                    10668, false, 231.5, 201.3, 0, nullptr, 0, "", false, 0})})}}
        .dump();
  });
  CHECK(p.poll_once());
  CHECK(asked.size() == 5);  // four adsb.lol hubs, then OpenSky
  CHECK(p.status()["source"] == "opensky");
  CHECK(store->latest("plane", std::nullopt, 300, now)["n"] == 1);
}

TEST_CASE("live routes: ingest, list with ETag, track, watch") {
  auto c = cl();
  CHECK(cl().Get("/v1/live/planes")->status == 200);  // empty list, not an error
  const double now = gsa::live::now_s();
  json body{{"kind", "ship"},
            {"source", "aisstream.io"},
            {"fixes", json::array({json{{"id", "601000001"}, {"name", "SA AGULHAS II"}, {"t", now - 60}, {"lon", 18.43}, {"lat", -33.91},
                                        {"spd", 0.1}, {"hdg", 12}, {"extra", {{"type", 60}}}},
                                   json{{"id", "601000001"}, {"t", now}, {"lon", 18.44}, {"lat", -33.90}},
                                   json{{"id", "bad"}, {"lon", 500}, {"lat", 0}, {"t", now}}})}};
  auto r = c.Post("/v1/live/ingest", body.dump(), "application/json");
  REQUIRE(r);
  CHECK(r->status == 200);
  CHECK(json::parse(r->body)["accepted"] == 2);

  r = c.Get("/v1/live/ships?bbox=18,-34.5,19,-33");
  REQUIRE(r->status == 200);
  auto j = json::parse(r->body);
  REQUIRE(j["n"] == 1);
  CHECK(j["items"][0]["name"] == "SA AGULHAS II");
  CHECK(j["items"][0]["type"] == 60);
  CHECK(j["sources"].contains("aisstream.io"));
  const std::string tag = r->get_header_value("ETag");
  CHECK(!tag.empty());
  CHECK(r->has_header("Last-Modified"));
  r = c.Get("/v1/live/ships?bbox=18,-34.5,19,-33", {{"If-None-Match", tag}});
  CHECK(r->status == 304);
  CHECK(r->body.empty());
  CHECK(c.Get("/v1/live/ships?bbox=1,2,3")->status == 400);

  j = json::parse(c.Get("/v1/live/ship/601000001/track")->body);
  CHECK(j["n"] == 2);
  CHECK(j["fixes"][1][1] == doctest::Approx(18.44));

  r = c.Put("/v1/live/watch/ship/601000001", R"({"label":"Agulhas"})", "application/json");
  CHECK(r->status == 201);
  j = json::parse(c.Get("/v1/live/watch")->body);
  REQUIRE(j.size() == 1);
  CHECK(j[0]["label"] == "Agulhas");
  CHECK(j[0]["last"]["lon"] == doctest::Approx(18.44));
  CHECK(json::parse(c.Get("/v1/live/ships")->body)["items"][0]["watched"] == true);
  CHECK(c.Delete("/v1/live/watch/ship/601000001")->status == 204);
  CHECK(c.Delete("/v1/live/watch/ship/601000001")->status == 404);
  CHECK(c.Post("/v1/live/ingest", R"({"kind":"train","fixes":[]})", "application/json")->status == 400);
  j = json::parse(c.Get("/v1/live/status")->body);
  CHECK(j["enabled"] == true);
  CHECK(j["store"]["kinds"]["ship"]["latest"] == 1);
}

TEST_CASE("caching: tile ETag, JSON ETag and gzip, response cache") {
  auto c = cl();
  auto t = c.Get("/v1/tiles/admin/0/0/0");
  REQUIRE(t);
  REQUIRE(t->status == 200);
  const std::string tag = t->get_header_value("ETag");
  CHECK(tag.find("admin-") != std::string::npos);
  CHECK(c.Get("/v1/tiles/admin/0/0/0", {{"If-None-Match", tag}})->status == 304);

  auto a = c.Get("/v1/areas/ZA");
  REQUIRE(a->status == 200);
  CHECK(!a->has_header("X-Cache"));
  auto b = c.Get("/v1/areas/ZA");
  CHECK(b->get_header_value("X-Cache") == "hit");
  CHECK(b->body == a->body);
  CHECK(b->get_header_value("ETag") == a->get_header_value("ETag"));
  CHECK(c.Get("/v1/areas/ZA", {{"If-None-Match", a->get_header_value("ETag")}})->status == 304);

  auto z = c.Get("/v1/areas/ZA", {{"Accept-Encoding", "gzip, deflate"}});
  REQUIRE(z->status == 200);
  if (a->body.size() >= 1024) {
    CHECK(z->get_header_value("Content-Encoding") == "gzip");
    const auto raw = gsa::gunzip(reinterpret_cast<const uint8_t*>(z->body.data()), z->body.size());
    CHECK(std::string(raw.begin(), raw.end()) == a->body);
    CHECK(z->body.size() < a->body.size());
  }
  CHECK(json::parse(c.Get("/v1/cache")->body)["hits"].get<int>() >= 2);
}

TEST_CASE("plane poller can be turned on and off over HTTP") {
  httplib::Server srv;
  auto store = std::make_shared<gsa::live::Store>(gsa::live::Options{});
  std::atomic<int> calls{0};
  auto poller = std::make_shared<gsa::live::PlanePoller>(
      store, [&](const std::string&) -> std::optional<std::string> { ++calls; return std::nullopt; }, 3600);
  gsa::ApiConfig cfg{std::make_shared<gsa::PackStore>(GSA_DEVDATA), "", ""};
  cfg.live = store;
  cfg.poller = poller;
  gsa::mount_api(srv, cfg);
  const int port = srv.bind_to_any_port("127.0.0.1");
  std::thread th([&] { srv.listen_after_bind(); });
  srv.wait_until_ready();
  httplib::Client c("127.0.0.1", port);
  CHECK(json::parse(c.Get("/v1/live/status")->body)["planes"]["running"] == false);
  auto r = c.Post("/v1/live/poller", R"({"on":true})", "application/json");
  REQUIRE(r);
  CHECK(r->status == 200);
  CHECK(json::parse(r->body)["running"] == true);
  for (int i = 0; i < 50 && calls < 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(calls >= 5);  // one round: four adsb.lol hubs and OpenSky
  CHECK(json::parse(c.Post("/v1/live/poller", R"({"on":false})", "application/json")->body)["running"] == false);
  CHECK(c.Post("/v1/live/poller", R"({"on":"yes"})", "application/json")->status == 400);
  CHECK(json::parse(c.Post("/v1/live/poller", R"({"on":true})", "application/json")->body)["running"] == true);  // restartable
  poller->stop();
  srv.stop();
  th.join();
}
