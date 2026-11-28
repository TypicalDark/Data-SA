// /v1/live/*: live planes and ships, their history and the watch list (contract: api/openapi.yaml).
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gsa/api.hpp"
#include "gsa/live.hpp"

namespace gsa {

using nlohmann::json;

namespace {

void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}
void send_error(httplib::Response& res, int status, const std::string& msg) { send_json(res, json{{"error", msg}}, status); }

std::string http_date(double t) {
  const std::time_t tt = std::time_t(t);
  std::tm g{};
#ifdef _WIN32
  gmtime_s(&g, &tt);
#else
  gmtime_r(&tt, &g);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &g);
  return buf;
}

bool num_param(const httplib::Request& req, httplib::Response& res, const char* name, double lo, double hi, double& out) {
  if (!req.has_param(name)) return true;
  try {
    size_t used = 0;
    const std::string v = req.get_param_value(name);
    const double d = std::stod(v, &used);
    if (used != v.size() || !std::isfinite(d) || d < lo || d > hi) throw std::out_of_range(name);
    out = d;
    return true;
  } catch (...) {
    send_error(res, 400, std::string(name) + " must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
    return false;
  }
}

bool bbox_param(const httplib::Request& req, httplib::Response& res, std::optional<live::Bbox>& out) {
  if (!req.has_param("bbox")) return true;
  double v[4];
  const std::string s = req.get_param_value("bbox");
  if (std::sscanf(s.c_str(), "%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3]) != 4 || v[0] > v[2] || v[1] > v[3] ||
      v[0] < -180 || v[2] > 180 || v[1] < -90 || v[3] > 90) {
    send_error(res, 400, "bbox must be minlon,minlat,maxlon,maxlat");
    return false;
  }
  out = live::Bbox{v[0], v[1], v[2], v[3]};
  return true;
}

}  // namespace

void mount_live_api(httplib::Server& srv, std::shared_ptr<live::Store> store, std::shared_ptr<live::PlanePoller> poller) {
  auto need = [store](httplib::Response& res) {
    if (store) return true;
    send_error(res, 503, "live feeds are off (start gsa-backend with --live)");
    return false;
  };

  // Latest positions. Cheap to poll: the ETag changes only when a feed brought something new.
  srv.Get(R"(/v1/live/(plane|ship)s)", [store, need](const httplib::Request& req, httplib::Response& res) {
    if (!need(res)) return;
    const std::string kind = req.matches[1];
    std::optional<live::Bbox> box;
    double max_age = kind == "plane" ? 300 : 1800;
    if (!bbox_param(req, res, box) || !num_param(req, res, "max_age", 1, 7 * 86400, max_age)) return;
    const double now = live::now_s();
    // the answer depends on the feed version, the query and (through max_age) the clock: bucket the clock to a minute
    const std::string tag = "\"" + kind + "-" + std::to_string(store->version(kind)) + "-" +
                            std::to_string(std::hash<std::string>{}(req.target)) + "-" + std::to_string(int64_t(now / 60)) + "\"";
    res.set_header("ETag", tag);
    res.set_header("Cache-Control", "no-cache");
    if (const double m = store->modified(kind); m > 0) res.set_header("Last-Modified", http_date(m));
    if (req.get_header_value("If-None-Match") == tag) {
      res.status = 304;
      return;
    }
    send_json(res, store->latest(kind, box, max_age, now));
  });

  // Position history of one plane or ship, oldest first.
  srv.Get(R"(/v1/live/(plane|ship)/([A-Za-z0-9_.-]{1,32})/track)", [store, need](const httplib::Request& req, httplib::Response& res) {
    if (!need(res)) return;
    const double now = live::now_s();
    double since = now - 86400, until = now + 60, limit = 5000;
    if (!num_param(req, res, "since", 0, 1e11, since) || !num_param(req, res, "until", 0, 1e11, until) ||
        !num_param(req, res, "limit", 1, 100000, limit))
      return;
    const json t = store->track(req.matches[1], req.matches[2], since, until, size_t(limit));
    res.set_header("Cache-Control", "no-cache");
    send_json(res, t);
  });

  srv.Get("/v1/live/watch", [store, need](const httplib::Request&, httplib::Response& res) {
    if (!need(res)) return;
    send_json(res, store->watch_list());
  });
  srv.Put(R"(/v1/live/watch/(plane|ship)/([A-Za-z0-9_.-]{1,32}))", [store, need](const httplib::Request& req, httplib::Response& res) {
    if (!need(res)) return;
    std::string label;
    if (!req.body.empty()) {
      const json b = json::parse(req.body, nullptr, false);
      if (b.is_discarded() || !b.is_object()) return send_error(res, 400, "body must be a JSON object such as {\"label\":\"SA Agulhas II\"}");
      if (b.contains("label") && b["label"].is_string()) label = b["label"].get<std::string>().substr(0, 80);
    }
    const bool fresh = store->watch(req.matches[1], req.matches[2], label, live::now_s());
    send_json(res, json{{"kind", std::string(req.matches[1])}, {"id", std::string(req.matches[2])}, {"watched", true}}, fresh ? 201 : 200);
  });
  srv.Delete(R"(/v1/live/watch/(plane|ship)/([A-Za-z0-9_.-]{1,32}))", [store, need](const httplib::Request& req, httplib::Response& res) {
    if (!need(res)) return;
    if (!store->unwatch(req.matches[1], req.matches[2])) return send_error(res, 404, "not on the watch list");
    res.status = 204;
  });

  // Feeders (tools/live/ais_feed.py for ships, or any other source) post batches here.
  auto last_prune = std::make_shared<std::atomic<double>>(0.0);
  srv.Post("/v1/live/ingest", [store, need, last_prune](const httplib::Request& req, httplib::Response& res) {
    if (!need(res)) return;
    const json b = json::parse(req.body, nullptr, false);
    if (b.is_discarded() || !b.is_object()) return send_error(res, 400, "body must be JSON");
    const std::string kind = b.value("kind", ""), source = b.value("source", "feeder");
    if (!live::valid_kind(kind)) return send_error(res, 400, "kind must be plane or ship");
    if (source.empty() || source.size() > 40) return send_error(res, 400, "source must be 1 to 40 characters");
    std::vector<live::Fix> fixes;
    try {
      fixes = live::parse_ingest(b);
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    const double now = live::now_s();
    const size_t stored = store->ingest(kind, fixes, source, now);
    if (now - last_prune->load() > 600) last_prune->store(now), store->prune(now);
    send_json(res, json{{"accepted", fixes.size()}, {"history", stored}});
  });

  // Turns the plane poller on or off at runtime (the app starts the back-end with --no-plane-poll and
  // switches polling on only when the user turns on live planes).
  srv.Post("/v1/live/poller", [poller](const httplib::Request& req, httplib::Response& res) {
    if (!poller) return send_error(res, 503, "live feeds are off (start gsa-backend with --live)");
    const json b = json::parse(req.body, nullptr, false);
    if (b.is_discarded() || !b.is_object() || !b.contains("on") || !b["on"].is_boolean())
      return send_error(res, 400, "body must be {\"on\": true} or {\"on\": false}");
    if (b["on"].get<bool>()) poller->start();
    else poller->stop();
    send_json(res, poller->status());
  });

  srv.Get("/v1/live/status", [store, poller](const httplib::Request&, httplib::Response& res) {
    json j{{"enabled", bool(store)}};
    if (store) j["store"] = store->status();
    if (poller) j["planes"] = poller->status();
    send_json(res, j);
  });
}

}  // namespace gsa
