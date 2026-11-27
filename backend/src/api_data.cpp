// /v1 routes over the SQLite pack's plot, time series and grid graph tables.
// Contract: api/openapi.yaml (paths /v1/parcels, /v1/series, /v1/graph, /v1/trace).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gsa/api.hpp"
#include "gsa/graph.hpp"

namespace gsa {

using nlohmann::json;
namespace {

void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}
void send_error(httplib::Response& res, int status, const std::string& msg) {
  send_json(res, json{{"error", msg}}, status);
}

bool has_table(sql::Db& db, const std::string& name) {
  bool found = false;
  db.query("SELECT 1 FROM sqlite_master WHERE type IN ('table','view') AND name = ?", {name},
           [&](const sql::Row&) { found = true; });
  return found;
}

bool has_column(sql::Db& db, const std::string& table, const std::string& column) {
  bool hit = false;
  db.query("SELECT 1 FROM pragma_table_info(?) WHERE name = ?", {table, column}, [&](const sql::Row&) { hit = true; });
  return hit;
}

// Optional integer query parameter; false (and a 400 already sent) when it is not a number in range.
bool int_param(const httplib::Request& req, httplib::Response& res, const char* name, int64_t lo, int64_t hi,
               std::optional<int64_t>& out) {
  if (!req.has_param(name)) return true;
  try {
    size_t used = 0;
    const std::string v = req.get_param_value(name);
    const long long x = std::stoll(v, &used);
    if (used != v.size() || x < lo || x > hi) throw std::out_of_range(name);
    out = x;
    return true;
  } catch (...) {
    send_error(res, 400, std::string(name) + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
    return false;
  }
}

// ------------------------------------------------------------------ graph cache
// One in-memory graph per (attribute pack, network). Keyed by the pool so a pack swap builds a new
// graph, and held weakly so a retired pack's graph goes with it.
class GraphCache {
 public:
  std::shared_ptr<const graph::Graph> get(const std::shared_ptr<sql::Pool>& pool, const std::string& network) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = items_.begin(); it != items_.end();) {
      if (it->pool.expired()) it = items_.erase(it);
      else ++it;
    }
    for (const auto& i : items_)
      if (i.pool.lock() == pool && i.network == network) return i.g;
    auto db = pool->lease();
    auto g = graph::Graph::load(*db, network);
    items_.push_back({pool, network, g});
    return g;
  }

 private:
  struct Item {
    std::weak_ptr<sql::Pool> pool;
    std::string network;
    std::shared_ptr<const graph::Graph> g;
  };
  std::mutex mu_;
  std::vector<Item> items_;
};

json trace_json(const graph::Trace& t) {
  return json{{"nodes", t.nodes}, {"edges", t.edges}, {"km", std::round(t.km * 100) / 100}, {"truncated", t.truncated}};
}

json node_json(const graph::Graph& g, int i) {
  const auto& n = g.node(i);
  json j{{"id", n.id}, {"kind", n.kind}, {"kv", n.kv}};
  if (!n.ext_id.empty()) j["ext_id"] = n.ext_id;
  if (!n.name.empty()) j["name"] = n.name;
  if (!n.op.empty()) j["operator"] = n.op;
  if (n.has_pos) j["center"] = {n.lon, n.lat};
  return j;
}

// ------------------------------------------------------------------ handlers
// The pack that holds a plot: the province packs first, then the national pack (a single-file
// install keeps plots there). Returns null when no installed pack has it.
std::shared_ptr<sql::Pool> plot_pack(const PackSet& set, const std::string& sg21) {
  std::vector<std::shared_ptr<sql::Pool>> pools;
  for (const auto& [region, pool] : set.regional) pools.push_back(pool);
  if (set.db) pools.push_back(set.db);
  for (const auto& pool : pools) {
    auto db = pool->lease();
    if (!has_table(*db, "parcel")) continue;
    bool hit = false;
    db->query("SELECT 1 FROM parcel WHERE sg21 = ?", {sg21}, [&](const sql::Row&) { hit = true; });
    if (hit) return pool;
  }
  return nullptr;
}

void handle_parcel(const PackSet& set, const std::string& sg21, httplib::Response& res) {
  const auto pool = plot_pack(set, sg21);
  if (!pool) {
    bool any = false;
    for (const auto& [region, p] : set.regional) any = any || has_table(*p->lease(), "parcel");
    if (!any && !has_table(*set.db->lease(), "parcel")) return send_error(res, 503, "the installed packs have no plots");
    return send_error(res, 404, "no plot " + sg21 + " in the installed provinces");
  }
  auto db = pool->lease();
  json out;
  std::string ward, suburb;
  const bool rich = has_column(*db, "parcel", "stand");  // packs built before migration 002 lack stand/superseded_by
  db->query(std::string("SELECT sg21, kind, town, parcel_no, portion_no, area_m2, ward_id, suburb_id, h3_r9, lon, lat") +
                (rich ? ", stand, superseded_by" : "") + " FROM parcel WHERE sg21 = ?",
            {sg21}, [&](const sql::Row& r) {
              out = json{{"sg21", r.text(0)}, {"kind", "PLOT"}};
              if (!r.is_null(1)) out["parcel_kind"] = r.text(1);
              if (!r.is_null(2)) out["town"] = r.text(2);
              if (!r.is_null(3)) out["parcel_no"] = r.text(3);
              if (!r.is_null(4)) out["portion_no"] = r.text(4);
              if (!r.is_null(5)) out["area_m2"] = r.f64(5);
              if (!r.is_null(8)) out["h3_r9"] = r.text(8);
              if (!r.is_null(9)) out["center"] = {r.f64(9), r.f64(10)};
              if (rich && !r.is_null(11)) out["stand"] = r.text(11);
              // an older SG parcel replaced by a newer metro stand: the map draws the stand instead
              if (rich && !r.is_null(12)) out["superseded_by"] = r.text(12);
              ward = r.opt_text(6).value_or("");
              suburb = r.opt_text(7).value_or("");
            });
  // the SG code is a tree too: registration division / township / parcel / portion
  out["sg"] = {{"division", sg21.substr(0, 4)}, {"township", sg21.substr(0, 8)}, {"parcel", sg21.substr(0, 16)}};

  // breadcrumb: the ward's chain from the place tree (national pack), then the suburb
  json crumbs = json::array();
  {
    auto tree = set.db->lease();
    std::string up = ward;
    for (int guard = 0; guard < 10 && !up.empty(); ++guard) {
      std::string next;
      tree->query("SELECT area_id, level, name, parent_id FROM tree WHERE area_id = ?", {up}, [&](const sql::Row& r) {
        crumbs.insert(crumbs.begin(), json{{"id", r.text(0)}, {"level", r.i64(1)}, {"name", r.opt_text(2).value_or("")}});
        next = r.opt_text(3).value_or("");
      });
      up = next;
    }
    if (!suburb.empty()) {
      tree->query("SELECT area_id, level, name FROM tree WHERE area_id = ?", {suburb}, [&](const sql::Row& r) {
        crumbs.push_back(json{{"id", r.text(0)}, {"level", r.i64(1)}, {"name", r.opt_text(2).value_or("")}});
      });
    }
  }
  out["breadcrumb"] = crumbs;
  out["ward_id"] = ward.empty() ? json(nullptr) : json(ward);
  out["suburb_id"] = suburb.empty() ? json(nullptr) : json(suburb);

  // value entries (several for sectional titles, or from more than one source); every one cites its source.
  // market_value sums the entries that count: not zero placeholders, not superseded by a higher-priority roll.
  json vals = json::array();
  double total = 0;
  bool any = false;
  if (has_table(*db, "valuation")) {
    const bool vrich = has_column(*db, "valuation", "value_flag");
    db->query(std::string("SELECT muni_code, roll_period, category, usage, extent_m2, market_value, sale_price, sale_date, address, "
                          "source_url") +
                  (vrich ? ", roll, zoning, value_flag" : "") + " FROM valuation WHERE sg21 = ? ORDER BY roll_period DESC, market_value DESC",
              {sg21}, [&](const sql::Row& r) {
                json v{{"muni_code", r.text(0)}};
                const char* keys[] = {"roll_period", "category", "usage"};
                for (int i = 0; i < 3; ++i)
                  if (!r.is_null(1 + i)) v[keys[i]] = r.text(1 + i);
                std::string flag;
                if (vrich) {
                  if (!r.is_null(10)) v["roll"] = r.text(10);
                  if (!r.is_null(11)) v["zoning"] = r.text(11);
                  flag = r.opt_text(12).value_or("");
                  if (!flag.empty()) v["flag"] = flag;
                }
                if (!r.is_null(4)) v["extent_m2"] = r.f64(4);
                if (!r.is_null(5)) {
                  v["market_value"] = r.f64(5);
                  if (flag != "zero" && flag != "superseded") total += r.f64(5), any = true;
                }
                if (!r.is_null(6)) v["sale_price"] = r.f64(6);
                if (!r.is_null(7)) v["sale_date"] = r.text(7);
                if (!r.is_null(8)) v["address"] = r.text(8);
                if (!r.is_null(9)) v["source_url"] = r.text(9);
                vals.push_back(v);
              });
  }
  out["valuations"] = vals;
  out["market_value"] = any ? json(total) : json(nullptr);
  send_json(res, out);
}

void handle_series(const PackSet& set, const httplib::Request& req, httplib::Response& res) {
  const std::string entity = req.get_param_value("entity");
  if (entity.empty() || entity.size() > 64) return send_error(res, 400, "entity is required (an area id such as W10503001, or a place id)");
  auto db = set.db->lease();
  if (!has_table(*db, "series")) return send_error(res, 503, "the installed pack has no time series");
  const std::string variable = req.get_param_value("variable");

  if (variable.empty()) {  // list what exists for this entity
    json out = json::array();
    db->query("SELECT series_id, variable, unit, step, source_id FROM series WHERE entity_id = ? ORDER BY variable", {entity},
              [&](const sql::Row& r) {
                json s{{"series_id", r.i64(0)}, {"variable", r.text(1)}, {"step", r.opt_text(3).value_or("")}};
                if (!r.is_null(2)) s["unit"] = r.text(2);
                if (!r.is_null(4)) s["source_id"] = r.text(4);
                out.push_back(s);
              });
    return send_json(res, out);
  }

  json out;
  bool found = false;
  db->query("SELECT s.series_id, s.unit, s.step, s.source_id, d.t0, d.n, d.v FROM series s "
            "LEFT JOIN series_data d ON d.series_id = s.series_id WHERE s.entity_id = ? AND s.variable = ? LIMIT 1",
            {entity, variable}, [&](const sql::Row& r) {
              found = true;
              out = json{{"series_id", r.i64(0)}, {"entity", entity}, {"variable", variable}, {"step", r.opt_text(2).value_or("")}};
              if (!r.is_null(1)) out["unit"] = r.text(1);
              if (!r.is_null(3)) out["source_id"] = r.text(3);
              out["t0"] = r.opt_text(4).value_or("");
              json vals = json::array();
              if (!r.is_null(6)) {
                const std::string blob = r.blob(6);
                const size_t n = blob.size() / 4;
                for (size_t i = 0; i < n; ++i) {
                  float f;
                  std::memcpy(&f, blob.data() + i * 4, 4);  // little-endian float32 (all our targets)
                  vals.push_back(std::isnan(f) ? json(nullptr) : json(std::round(double(f) * 100) / 100));
                }
              }
              out["values"] = vals;
            });
  if (!found) return send_error(res, 404, "no series " + variable + " for " + entity);
  send_json(res, out);
}

void handle_graph_node(GraphCache& cache, const PackSet& set, const std::string& network, int64_t id, httplib::Response& res) {
  const auto g = cache.get(set.db, network);
  const auto i = g->node_index(id);
  if (!i) return send_error(res, 404, "no node " + std::to_string(id) + " in " + network);
  json out = node_json(*g, *i);
  json edges = json::array();
  for (const auto& h : g->hops(*i)) {
    const auto& e = g->edge(h.edge);
    json ej{{"id", e.id}, {"other", g->node(h.other).id}, {"kv", e.kv}, {"km", std::round(e.cost * 100) / 100}, {"inferred", e.inferred}};
    if (!e.cls.empty()) ej["cls"] = e.cls;
    edges.push_back(ej);
  }
  out["edges"] = edges;
  out["network"] = network;
  send_json(res, out);
}

void handle_trace(GraphCache& cache, const PackSet& set, const httplib::Request& req, httplib::Response& res) {
  const std::string network = req.has_param("network") ? req.get_param_value("network") : "power";
  const std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "connected";
  std::optional<int64_t> from, to, edge, min_kv, source_kv, limit, k;
  if (!int_param(req, res, "from", 0, INT64_MAX, from) || !int_param(req, res, "to", 0, INT64_MAX, to) ||
      !int_param(req, res, "edge", 0, INT64_MAX, edge) || !int_param(req, res, "min_kv", 0, 2000, min_kv) ||
      !int_param(req, res, "source_kv", 1, 2000, source_kv) || !int_param(req, res, "limit", 1, 200000, limit) ||
      !int_param(req, res, "k", 1, 20, k))
    return;
  const auto g = cache.get(set.db, network);
  if (g->node_count() == 0) return send_error(res, 404, "no network " + network + " in the installed pack");
  auto node = [&](const std::optional<int64_t>& id, const char* what) -> std::optional<int> {
    if (!id) return std::nullopt;
    auto i = g->node_index(*id);
    if (!i) send_error(res, 404, std::string("no ") + what + " node " + std::to_string(*id));
    return i;
  };
  json out{{"mode", mode}, {"network", network}};
  const size_t lim = size_t(limit.value_or(20000));

  if (mode == "connected" || mode == "path" || mode == "feed") {
    if (!from) return send_error(res, 400, "from is required");
    const auto fi = node(from, "from");
    if (!fi) return;
    out["from"] = node_json(*g, *fi);
    if (mode == "connected") {
      out.update(trace_json(g->connected(*fi, int(min_kv.value_or(0)), lim)));
    } else if (mode == "path") {
      if (!to) return send_error(res, 400, "to is required for mode=path");
      const auto ti = node(to, "to");
      if (!ti) return;
      const auto t = g->path(*fi, *ti, int(min_kv.value_or(0)));
      out["to"] = node_json(*g, *ti);
      out["found"] = t.found;
      out.update(trace_json(t));
    } else {
      json srcs = json::array();
      for (const auto& s : g->feed(*fi, int(source_kv.value_or(220)), size_t(k.value_or(3)))) {
        json sj = node_json(*g, *g->node_index(s.node));
        sj["km"] = std::round(s.km * 100) / 100;
        sj["path"] = trace_json(s.path);
        srcs.push_back(sj);
      }
      out["source_kv"] = source_kv.value_or(220);
      out["sources"] = srcs;
    }
    return send_json(res, out);
  }
  if (mode == "fail") {
    std::optional<int> fn, fe;
    if (from) {
      fn = node(from, "from");
      if (!fn) return;
      out["failed_node"] = node_json(*g, *fn);
    } else if (edge) {
      fe = g->edge_index(*edge);
      if (!fe) return send_error(res, 404, "no edge " + std::to_string(*edge));
      out["failed_edge"] = *edge;
    } else {
      return send_error(res, 400, "mode=fail needs from (a node) or edge");
    }
    const auto t = g->fail(fn, fe, int(source_kv.value_or(220)), lim);
    out["source_kv"] = source_kv.value_or(220);
    out["affected"] = trace_json(t);
    return send_json(res, out);
  }
  send_error(res, 400, "mode must be connected, path, feed or fail");
}

bool need_db(const PackSet& set, httplib::Response& res) {
  if (set.db) return true;
  send_error(res, 503, "no attribute pack installed");
  return false;
}

}  // namespace

void mount_data_api(httplib::Server& srv, std::shared_ptr<PackStore> store) {
  auto cache = std::make_shared<GraphCache>();

  srv.Get(R"(/v1/parcels/([A-Z][0-9A-Z]{20}))", [store](const httplib::Request& req, httplib::Response& res) {
    const auto set = store->current();
    if (need_db(*set, res)) handle_parcel(*set, req.matches[1], res);
  });

  srv.Get("/v1/series", [store](const httplib::Request& req, httplib::Response& res) {
    const auto set = store->current();
    if (need_db(*set, res)) handle_series(*set, req, res);
  });

  srv.Get(R"(/v1/graph/([a-z]{1,16})/nodes/(\d{1,18}))", [store, cache](const httplib::Request& req, httplib::Response& res) {
    const auto set = store->current();
    if (need_db(*set, res)) handle_graph_node(*cache, *set, req.matches[1], std::stoll(req.matches[2]), res);
  });

  srv.Get("/v1/trace", [store, cache](const httplib::Request& req, httplib::Response& res) {
    const auto set = store->current();
    if (need_db(*set, res)) handle_trace(*cache, *set, req, res);
  });
}

}  // namespace gsa
