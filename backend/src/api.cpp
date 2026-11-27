#include "gsa/api.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace gsa {

using nlohmann::json;
namespace {

const auto kStarted = std::chrono::steady_clock::now();

void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}
void send_error(httplib::Response& res, int status, const std::string& msg) {
  send_json(res, json{{"error", msg}}, status);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

json pack_json(const PackInfo& p, const PackSet& set) {
  json j{{"id", p.id}, {"kind", p.kind}, {"version", p.version}, {"bytes", p.bytes}};
  if (!p.sha256.empty()) j["sha256"] = p.sha256;
  if (!p.region.empty()) j["region"] = p.region;
  if (p.kind != "tiles") return j;
  const auto& a = *set.tiles.at(p.id);
  const auto& h = a.header();
  j["tile_type"] = pmtiles::type_name(h.tile_type);
  j["minzoom"] = h.min_zoom;
  j["maxzoom"] = h.max_zoom;
  j["bounds"] = {h.bounds[0], h.bounds[1], h.bounds[2], h.bounds[3]};
  const auto& m = a.metadata();
  if (m.contains("name") && m["name"].is_string()) j["title"] = m["name"];
  json layers = json::array();
  if (m.contains("vector_layers") && m["vector_layers"].is_array()) {
    for (const auto& l : m["vector_layers"]) {
      json o{{"id", l.value("id", "")}};
      if (l.contains("minzoom")) o["minzoom"] = l["minzoom"];
      if (l.contains("maxzoom")) o["maxzoom"] = l["maxzoom"];
      if (l.contains("fields")) o["fields"] = l["fields"];
      layers.push_back(o);
    }
  }
  j["layers"] = layers;
  return j;
}

struct Ctx {
  ApiConfig cfg;
  std::shared_ptr<const PackSet> packs() const { return cfg.store->current(); }
};

bool need_db(const PackSet& set, httplib::Response& res) {
  if (set.db) return true;
  send_error(res, 503, "no attribute pack installed");
  return false;
}

json area_ref(const std::string& id, int level, const std::string& name) {
  return json{{"id", id}, {"level", level}, {"kind", kind_for_level(level)}, {"name", name}};
}

void handle_area(const Ctx& ctx, const std::string& id, httplib::Response& res) {
  const auto set = ctx.packs();
  if (!need_db(*set, res)) return;
  auto db = set->db->lease();
  json out;
  bool found = false;
  db->query(
      "SELECT area_id, level, parent_id, name, minx, miny, maxx, maxy, lon, lat, area_km2 FROM tree WHERE area_id = ?",
      {id}, [&](const sql::Row& r) {
        found = true;
        out = area_ref(r.text(0), int(r.i64(1)), r.text(3));
        out["parent_id"] = r.is_null(2) ? json(nullptr) : json(r.text(2));
        if (!r.is_null(4)) out["bbox"] = {r.f64(4), r.f64(5), r.f64(6), r.f64(7)};
        if (!r.is_null(8)) out["center"] = {r.f64(8), r.f64(9)};
        out["area_km2"] = r.is_null(10) ? json(nullptr) : json(r.f64(10));
      });
  if (!found) return send_error(res, 404, "no area " + id);

  // breadcrumb: walk up the parents (the tree is at most 8 deep)
  json crumbs = json::array();
  std::string up = out["parent_id"].is_null() ? std::string() : out["parent_id"].get<std::string>();
  for (int guard = 0; guard < 10 && !up.empty(); ++guard) {
    std::string next;
    db->query("SELECT area_id, level, name, parent_id FROM tree WHERE area_id = ?", {up}, [&](const sql::Row& r) {
      crumbs.insert(crumbs.begin(), area_ref(r.text(0), int(r.i64(1)), r.text(2)));
      next = r.is_null(3) ? std::string() : r.text(3);
    });
    up = next;
  }
  out["breadcrumb"] = crumbs;

  json metrics = json::array();
  db->query("SELECT metric, as_of, v, n, source_id FROM metric WHERE area_id = ? ORDER BY metric, as_of DESC", {id},
            [&](const sql::Row& r) {
              json m{{"metric", r.text(0)}, {"v", r.f64(2)}};
              if (!r.is_null(1)) m["as_of"] = r.text(1);
              if (!r.is_null(3)) m["n"] = r.i64(3);
              if (!r.is_null(4)) m["source_id"] = r.text(4);
              metrics.push_back(m);
            });
  out["metrics"] = metrics;

  std::map<std::string, json> kids;
  std::vector<std::string> order;
  db->query("SELECT area_id, level, name, lon, lat, area_km2 FROM tree WHERE parent_id = ? ORDER BY name", {id},
            [&](const sql::Row& r) {
    auto c = area_ref(r.text(0), int(r.i64(1)), r.text(2));
    if (!r.is_null(3)) c["center"] = {r.f64(3), r.f64(4)};
    if (!r.is_null(5)) c["area_km2"] = r.f64(5);
    c["metrics"] = json::object();
    order.push_back(r.text(0));
    kids.emplace(r.text(0), std::move(c));
  });
  if (!kids.empty()) {
    // latest value of the headline metrics for every child, oldest first so the newest wins
    db->query(
        "SELECT m.area_id, m.metric, m.v FROM metric m JOIN tree t ON t.area_id = m.area_id "
        "WHERE t.parent_id = ? AND m.metric IN ('pop_est','value_median','parcels','households') ORDER BY m.as_of",
        {id}, [&](const sql::Row& r) {
          if (auto it = kids.find(r.text(0)); it != kids.end()) it->second["metrics"][r.text(1)] = r.f64(2);
        });
  }
  json children = json::array();
  for (const auto& k : order) children.push_back(std::move(kids[k]));
  out["children"] = children;
  send_json(res, out);
}

void handle_search(const Ctx& ctx, const httplib::Request& req, httplib::Response& res) {
  const std::string q = req.get_param_value("q");
  if (q.empty() || q.size() > 200) return send_error(res, 400, "q must be 1 to 200 characters");
  int limit = 12;
  if (req.has_param("limit")) {
    try {
      limit = std::clamp(std::stoi(req.get_param_value("limit")), 1, 50);
    } catch (...) {
      return send_error(res, 400, "limit must be a number");
    }
  }
  const auto set = ctx.packs();
  if (!need_db(*set, res)) return;
  const std::string fq = fts_query(q);
  json out = json::array();
  if (fq.empty()) return send_json(res, out);
  // Ranking: exact label first, then label prefix, then bm25 text relevance, nudged by the
  // pack's weight (provinces and municipalities above wards, wards above plots). The national pack
  // and every province pack (plots) are searched, and the best hits of all of them are merged.
  const std::string order =
      "(CASE WHEN lower(label) = ?2 THEN -100 WHEN lower(label) LIKE ?2 || '%' THEN -20 ELSE 0 END) "
      "+ bm25(search) - coalesce(weight, 0) * 1.5";
  struct Hit {
    double score;
    json j;
  };
  std::vector<Hit> hits;
  auto run = [&](sql::Pool& pool, bool national) {
    auto db = pool.lease();
    if (!national) {
      bool fts = false;
      db->query("SELECT 1 FROM sqlite_master WHERE name = 'search'", {}, [&](const sql::Row&) { fts = true; });
      if (!fts) return;
    }
    const std::string level = national
        ? "CASE WHEN ref LIKE 'area:%' THEN (SELECT level FROM tree WHERE area_id = substr(search.ref, 6)) END"
        : "NULL";
    const std::string sql = "SELECT ref, label, detail, lon, lat, " + level + " AS level, " + order +
                            " AS score FROM search WHERE search MATCH ?1 ORDER BY score LIMIT ?3";
    db->query(sql, {fq, lower(q), int64_t(limit)}, [&](const sql::Row& r) {
      const std::string ref = r.text(0), detail = r.text(2);
      std::optional<int> lv;
      if (!r.is_null(5)) lv = int(r.i64(5));
      json h{{"ref", ref}, {"kind", kind_for(ref, lv, detail)}, {"label", r.text(1)}, {"detail", detail}};
      if (!r.is_null(3)) h["lon"] = r.f64(3);
      if (!r.is_null(4)) h["lat"] = r.f64(4);
      hits.push_back({r.f64(6), std::move(h)});
    });
  };
  run(*set->db, true);
  for (const auto& [region, pool] : set->regional) run(*pool, false);
  std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score < b.score; });
  for (auto& h : hits) {
    if (int(out.size()) >= limit) break;
    out.push_back(std::move(h.j));
  }
  send_json(res, out);
}

void handle_tile(const Ctx& ctx, const httplib::Request& req, httplib::Response& res) {
  const std::string pack = req.matches[1];
  uint32_t z, x, y;
  try {
    const unsigned long zz = std::stoul(req.matches[2]), xx = std::stoul(req.matches[3]), yy = std::stoul(req.matches[4]);
    if (zz > 24 || xx >= (1ul << zz) || yy >= (1ul << zz)) return send_error(res, 400, "tile address out of range");
    z = uint32_t(zz), x = uint32_t(xx), y = uint32_t(yy);
  } catch (...) {
    return send_error(res, 400, "bad tile address");
  }
  const auto set = ctx.packs();
  const auto it = set->tiles.find(pack);
  if (it == set->tiles.end()) return send_error(res, 404, "no tile pack " + pack);
  const auto tile = it->second->tile(uint8_t(z), x, y);
  res.set_header("Cache-Control", "private, max-age=3600");
  if (!tile) {
    res.status = 204;
    return;
  }
  if (const char* enc = pmtiles::content_encoding(tile->compression)) res.set_header("Content-Encoding", enc);
  res.status = 200;
  res.set_content(reinterpret_cast<const char*>(tile->data.data()), tile->data.size(),
                  pmtiles::content_type(tile->tile_type));
}

}  // namespace

std::string fts_query(const std::string& q) {
  std::string out, word;
  auto flush = [&] {
    if (word.empty()) return;
    if (!out.empty()) out += ' ';
    out += '"' + word + "\"*";
    word.clear();
  };
  for (unsigned char c : q) {
    // letters and digits (and any UTF-8 byte, so accented names still match) form words;
    // everything else, including FTS5 syntax, separates them
    if (std::isalnum(c) || c >= 0x80) word += char(std::tolower(c));
    else flush();
    if (out.size() > 400) break;
  }
  flush();
  return out;
}

std::string kind_for_level(int level) {
  switch (level) {
    case 0: return "COUNTRY";
    case 1: return "PROV";
    case 2: return "DIST";
    case 3: return "MUNI";
    case 4: return "WARD";
    case 5: return "PLACE";
    case 6: return "SUBURB";
    case 7: return "PLOT";
    default: return "AREA";
  }
}

std::string kind_for(const std::string& ref, std::optional<int> level, const std::string& detail) {
  auto starts = [&](const char* p) { return ref.rfind(p, 0) == 0; };
  if (starts("area:")) return level ? kind_for_level(*level) : "AREA";
  if (starts("place:")) return "TOWN";
  if (starts("parcel:")) return "PLOT";
  if (starts("node:power:")) {
    const auto d = lower(detail);
    if (d.rfind("substation", 0) == 0) return "SUB";
    if (d.find("power station") != std::string::npos || d.find("power_station") != std::string::npos ||
        d.find("plant") != std::string::npos)
      return "PLANT";
    return "SUB";
  }
  return "SPOT";
}

bool token_equal(const std::string& a, const std::string& b) {
  unsigned char diff = a.size() == b.size() ? 0 : 1;
  const size_t n = std::max(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const unsigned char ca = i < a.size() ? a[i] : 0, cb = i < b.size() ? b[i] : 0;
    diff |= ca ^ cb;
  }
  return diff == 0;
}

void mount_api(httplib::Server& srv, ApiConfig cfg) {
  auto ctx = std::make_shared<Ctx>(Ctx{std::move(cfg)});

  srv.set_pre_routing_handler([ctx](const httplib::Request& req, httplib::Response& res) {
    if (!ctx->cfg.allowed_origin.empty()) {
      res.set_header("Access-Control-Allow-Origin", ctx->cfg.allowed_origin);
      res.set_header("Access-Control-Allow-Headers", "Authorization");
      res.set_header("Vary", "Origin");
    }
    if (req.method == "OPTIONS") {
      res.status = 204;
      return httplib::Server::HandlerResponse::Handled;
    }
    if (req.path.rfind("/v1/", 0) != 0 || req.path == "/v1/health" || ctx->cfg.token.empty()) {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    const std::string auth = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.rfind(prefix, 0) == 0 && token_equal(auth.substr(prefix.size()), ctx->cfg.token)) {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    send_error(res, 401, "missing or wrong token");
    return httplib::Server::HandlerResponse::Handled;
  });

  srv.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
    std::string msg = "internal error";
    try {
      if (ep) std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      msg = e.what();
    } catch (...) {
    }
    send_error(res, 500, msg);
  });

  srv.Get("/v1/health", [](const httplib::Request&, httplib::Response& res) {
    const auto up = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - kStarted);
    send_json(res, json{{"status", "ok"}, {"uptime_s", up.count()}});
  });

  srv.Get("/v1/version", [ctx](const httplib::Request&, httplib::Response& res) {
    json packs = json::object();
    for (const auto& p : ctx->packs()->info) packs[p.id] = p.version;
    send_json(res, json{{"api", kApiVersion}, {"backend", kBackendVersion}, {"pack_schema", kPackSchemas}, {"packs", packs}});
  });

  srv.Get("/v1/packs", [ctx](const httplib::Request&, httplib::Response& res) {
    const auto set = ctx->packs();
    json out = json::array();
    for (const auto& p : set->info) out.push_back(pack_json(p, *set));
    send_json(res, out);
  });

  srv.Get(R"(/v1/tilejson/([a-z0-9][a-z0-9_-]{0,63}))", [ctx](const httplib::Request& req, httplib::Response& res) {
    const std::string id = req.matches[1];
    const auto set = ctx->packs();
    const auto it = set->tiles.find(id);
    if (it == set->tiles.end()) return send_error(res, 404, "no tile pack " + id);
    const auto& h = it->second->header();
    const auto& m = it->second->metadata();
    json tj{{"tilejson", "3.0.0"},
            {"name", m.value("name", id)},
            {"tiles", {"/v1/tiles/" + id + "/{z}/{x}/{y}"}},
            {"minzoom", h.min_zoom},
            {"maxzoom", h.max_zoom},
            {"bounds", {h.bounds[0], h.bounds[1], h.bounds[2], h.bounds[3]}},
            {"center", {h.center[0], h.center[1], h.center_zoom}}};
    if (m.contains("vector_layers")) tj["vector_layers"] = m["vector_layers"];
    if (m.contains("attribution")) tj["attribution"] = m["attribution"];
    send_json(res, tj);
  });

  srv.Get(R"(/v1/tiles/([a-z0-9][a-z0-9_-]{0,63})/(\d{1,2})/(\d{1,8})/(\d{1,8})(?:\.(?:mvt|pbf|png|jpg|webp))?)",
          [ctx](const httplib::Request& req, httplib::Response& res) { handle_tile(*ctx, req, res); });

  srv.Get(R"(/v1/data/([a-z0-9][a-z0-9_-]{0,63}))", [ctx](const httplib::Request& req, httplib::Response& res) {
    const std::string id = req.matches[1];
    const auto set = ctx->packs();
    const auto it = set->data.find(id);
    if (it == set->data.end()) return send_error(res, 404, "no data pack " + id);
    res.set_header("Cache-Control", "private, max-age=3600");
    res.set_content(*it->second, "application/json");
  });

  mount_data_api(srv, ctx->cfg.store);  // plots, time series, grid graph (api_data.cpp)

  srv.Get("/v1/search", [ctx](const httplib::Request& req, httplib::Response& res) { handle_search(*ctx, req, res); });

  srv.Get(R"(/v1/areas/([A-Za-z0-9_.-]{1,40}))", [ctx](const httplib::Request& req, httplib::Response& res) {
    handle_area(*ctx, req.matches[1], res);
  });

  srv.Get("/v1/sources", [ctx](const httplib::Request&, httplib::Response& res) {
    const auto set = ctx->packs();
    if (!need_db(*set, res)) return;
    json out = json::array();
    auto db = set->db->lease();
    db->query("SELECT source_id, title, publisher, url, licence, resale_ok, cadence FROM source ORDER BY source_id", {},
              [&](const sql::Row& r) {
                json s{{"source_id", r.text(0)}, {"title", r.text(1)}};
                if (!r.is_null(2)) s["publisher"] = r.text(2);
                if (!r.is_null(3)) s["url"] = r.text(3);
                if (!r.is_null(4)) s["licence"] = r.text(4);
                if (!r.is_null(5)) s["resale_ok"] = r.i64(5) != 0;
                if (!r.is_null(6)) s["cadence"] = r.text(6);
                out.push_back(s);
              });
    send_json(res, out);
  });

  srv.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.body.empty() && res.status >= 400) send_error(res, res.status, httplib::status_message(res.status));
  });
}

}  // namespace gsa
