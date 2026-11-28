#include "gsa/live.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <sqlite3.h>

namespace gsa::live {

using nlohmann::json;

namespace {

constexpr double kPi = 3.14159265358979323846;

double metres(double lon1, double lat1, double lon2, double lat2) {
  const double k = kPi / 180, x = (lon2 - lon1) * k * std::cos((lat1 + lat2) / 2 * k), y = (lat2 - lat1) * k;
  return std::sqrt(x * x + y * y) * 6371000.0;
}

std::optional<double> num(const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return std::nullopt;
  const double v = it->get<double>();
  if (!std::isfinite(v)) return std::nullopt;
  return v;
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && s[i] == ' ') ++i;
  return s.substr(i);
}

bool sane(const Fix& f) {
  return !f.id.empty() && f.id.size() <= 32 && std::isfinite(f.lon) && std::isfinite(f.lat) && f.lon >= -180 &&
         f.lon <= 180 && f.lat >= -90 && f.lat <= 90 && std::isfinite(f.t) && f.t > 0;
}

sql::Value opt(const std::optional<double>& v) { return v ? sql::Value(*v) : sql::Value(); }

json item_json(const Fix& f, const std::string& source, bool watched) {
  json j = f.extra.is_object() ? f.extra : json::object();
  j["id"] = f.id;
  if (!f.name.empty()) j["name"] = f.name;
  j["lon"] = f.lon;
  j["lat"] = f.lat;
  j["t"] = std::round(f.t);
  if (f.alt) j["alt"] = *f.alt;
  if (f.spd) j["spd"] = *f.spd;
  if (f.hdg) j["hdg"] = *f.hdg;
  j["src"] = source;
  if (watched) j["watched"] = true;
  return j;
}

}  // namespace

double now_s() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool valid_kind(const std::string& kind) { return kind == "plane" || kind == "ship"; }

// ------------------------------------------------------------------ Store
Store::Store(Options opt) : opt_(std::move(opt)) {
  if (!opt_.db.empty()) std::filesystem::create_directories(opt_.db.parent_path());
  db_ = std::make_unique<sql::Db>(opt_.db.empty() ? std::filesystem::path(":memory:") : opt_.db, false);
  db_->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
            "CREATE TABLE IF NOT EXISTS pos (kind TEXT, id TEXT, t INTEGER, lon REAL, lat REAL, alt REAL, spd REAL, hdg REAL,"
            "  PRIMARY KEY (kind, id, t)) WITHOUT ROWID;"
            "CREATE INDEX IF NOT EXISTS pos_t ON pos (t);"
            "CREATE TABLE IF NOT EXISTS watch (kind TEXT, id TEXT, label TEXT, added INTEGER, PRIMARY KEY (kind, id));"
            "CREATE TABLE IF NOT EXISTS live_info (k TEXT PRIMARY KEY, v TEXT);"
            "INSERT OR IGNORE INTO live_info VALUES ('format', 'gsa-live/1');");
  db_->query("SELECT kind, id, label, added FROM watch", {}, [&](const sql::Row& r) {
    watch_[{r.text(0), r.text(1)}] = json{{"kind", r.text(0)}, {"id", r.text(1)}, {"label", r.text(2)}, {"added", r.i64(3)}};
  });
  db_->query("SELECT count(*) FROM pos", {}, [&](const sql::Row& r) { hist_rows_ = uint64_t(r.i64(0)); });
}

Store::~Store() = default;

size_t Store::ingest(const std::string& kind, const std::vector<Fix>& fixes, const std::string& source, double now) {
  if (!valid_kind(kind)) throw std::invalid_argument("kind must be plane or ship");
  std::lock_guard<std::mutex> lk(mu_);
  auto& k = kinds_[kind];
  size_t stored = 0;
  bool changed = false;
  db_->exec("BEGIN");
  try {
    for (const auto& f : fixes) {
      if (!sane(f) || f.t > now + 300) continue;  // garbage or clock far ahead
      auto& it = k.items[f.id];
      if (!it.fix.id.empty() && f.t <= it.fix.t) continue;  // older than what we have
      Fix keep = f;
      if (keep.name.empty()) keep.name = it.fix.name;  // AIS position reports come without the name
      if (it.fix.extra.is_object() && keep.extra.is_object())
        for (const auto& [ek, ev] : it.fix.extra.items())
          if (!keep.extra.contains(ek)) keep.extra[ek] = ev;
      it.fix = std::move(keep);
      it.source = source;
      changed = true;
      const bool moved = metres(it.hist_lon, it.hist_lat, f.lon, f.lat) >= opt_.min_move_m;
      if (f.t - it.hist_t >= double(opt_.min_gap_s) || (moved && f.t - it.hist_t >= 5)) {
        db_->query("INSERT OR REPLACE INTO pos VALUES (?,?,?,?,?,?,?,?)",
                   {kind, f.id, int64_t(std::llround(f.t)), f.lon, f.lat, opt(f.alt), opt(f.spd), opt(f.hdg)},
                   [](const sql::Row&) {});
        it.hist_t = f.t, it.hist_lon = f.lon, it.hist_lat = f.lat;
        ++stored;
      }
    }
    db_->exec("COMMIT");
  } catch (...) {
    db_->exec("ROLLBACK");
    throw;
  }
  hist_rows_ += stored;
  k.sources[source] = now;
  if (changed) {
    ++k.version;
    k.modified = now;
  }
  return stored;
}

json Store::latest(const std::string& kind, const std::optional<Bbox>& box, double max_age_s, double now) const {
  std::lock_guard<std::mutex> lk(mu_);
  json items = json::array();
  json sources = json::object();
  const auto kit = kinds_.find(kind);
  if (kit != kinds_.end()) {
    for (const auto& [id, it] : kit->second.items) {
      if (now - it.fix.t > max_age_s) continue;
      if (box && !box->contains(it.fix.lon, it.fix.lat)) continue;
      items.push_back(item_json(it.fix, it.source, watch_.count({kind, id}) > 0));
    }
    for (const auto& [s, t] : kit->second.sources) sources[s] = std::round(t);
  }
  return json{{"kind", kind}, {"t", std::round(now)}, {"n", items.size()}, {"sources", sources}, {"items", std::move(items)}};
}

uint64_t Store::version(const std::string& kind) const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = kinds_.find(kind);
  return it == kinds_.end() ? 0 : it->second.version;
}

double Store::modified(const std::string& kind) const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = kinds_.find(kind);
  return it == kinds_.end() ? 0 : it->second.modified;
}

json Store::track(const std::string& kind, const std::string& id, double since, double until, size_t limit) const {
  std::lock_guard<std::mutex> lk(mu_);
  json fixes = json::array();
  std::vector<json> rows;
  db_->query("SELECT t, lon, lat, alt, spd, hdg FROM pos WHERE kind = ? AND id = ? AND t >= ? AND t <= ? ORDER BY t DESC LIMIT ?",
             {kind, id, int64_t(std::floor(since)), int64_t(std::ceil(until)), int64_t(limit)}, [&](const sql::Row& r) {
               json p = json::array({r.i64(0), r.f64(1), r.f64(2)});
               for (int c = 3; c < 6; ++c) p.push_back(r.is_null(c) ? json(nullptr) : json(r.f64(c)));
               rows.push_back(std::move(p));
             });
  std::reverse(rows.begin(), rows.end());
  for (auto& r : rows) fixes.push_back(std::move(r));
  std::string name;
  // the newest fix may not have made it into the thinned history yet
  if (const auto kit = kinds_.find(kind); kit != kinds_.end()) {
    if (const auto it = kit->second.items.find(id); it != kit->second.items.end()) {
      const Fix& f = it->second.fix;
      name = f.name;
      const double last = fixes.empty() ? -1 : fixes.back()[0].get<double>();
      if (f.t > last + 0.5 && f.t >= since && f.t <= until && fixes.size() < limit) {
        json p = json::array({std::llround(f.t), f.lon, f.lat});
        for (const auto* v : {&f.alt, &f.spd, &f.hdg}) p.push_back(*v ? json(**v) : json(nullptr));
        fixes.push_back(std::move(p));
      }
    }
  }
  json out{{"kind", kind}, {"id", id}, {"watched", watch_.count({kind, id}) > 0}, {"n", fixes.size()}, {"fixes", std::move(fixes)}};
  if (!name.empty()) out["name"] = name;
  return out;
}

json Store::watch_list() const {
  std::lock_guard<std::mutex> lk(mu_);
  json out = json::array();
  for (const auto& [key, w] : watch_) {
    json j = w;
    if (const auto kit = kinds_.find(key.first); kit != kinds_.end())
      if (const auto it = kit->second.items.find(key.second); it != kit->second.items.end())
        j["last"] = item_json(it->second.fix, it->second.source, true);
    out.push_back(std::move(j));
  }
  return out;
}

bool Store::watch(const std::string& kind, const std::string& id, const std::string& label, double now) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto key = std::make_pair(kind, id);
  const bool fresh = watch_.count(key) == 0;
  watch_[key] = json{{"kind", kind}, {"id", id}, {"label", label}, {"added", std::llround(now)}};
  db_->query("INSERT OR REPLACE INTO watch VALUES (?,?,?,?)", {kind, id, label, int64_t(std::llround(now))}, [](const sql::Row&) {});
  return fresh;
}

bool Store::unwatch(const std::string& kind, const std::string& id) {
  std::lock_guard<std::mutex> lk(mu_);
  db_->query("DELETE FROM watch WHERE kind = ? AND id = ?", {kind, id}, [](const sql::Row&) {});
  return watch_.erase({kind, id}) > 0;
}

bool Store::watched(const std::string& kind, const std::string& id) const {
  std::lock_guard<std::mutex> lk(mu_);
  return watch_.count({kind, id}) > 0;
}

size_t Store::prune(double now) {
  std::lock_guard<std::mutex> lk(mu_);
  db_->query("DELETE FROM pos WHERE t < ? AND (kind, id) NOT IN (SELECT kind, id FROM watch)",
             {int64_t(now) - opt_.keep_s}, [](const sql::Row&) {});
  size_t n = size_t(sqlite3_changes(db_->raw()));
  db_->query("DELETE FROM pos WHERE t < ?", {int64_t(now) - opt_.keep_watched_s}, [](const sql::Row&) {});
  n += size_t(sqlite3_changes(db_->raw()));
  hist_rows_ = hist_rows_ > n ? hist_rows_ - n : 0;
  for (auto& [kind, k] : kinds_) {
    for (auto it = k.items.begin(); it != k.items.end();) {
      if (now - it->second.fix.t > double(opt_.stale_s)) it = k.items.erase(it), ++k.version;
      else ++it;
    }
  }
  return n;
}

json Store::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  json kinds = json::object();
  for (const auto& [kind, k] : kinds_) {
    json src = json::object();
    for (const auto& [s, t] : k.sources) src[s] = std::round(t);
    kinds[kind] = json{{"latest", k.items.size()}, {"version", k.version}, {"modified", std::round(k.modified)}, {"sources", src}};
  }
  return json{{"kinds", kinds},
              {"history_rows", hist_rows_},
              {"watched", watch_.size()},
              {"keep_s", opt_.keep_s},
              {"keep_watched_s", opt_.keep_watched_s},
              {"db", opt_.db.empty() ? std::string("memory") : opt_.db.string()}};
}

// ------------------------------------------------------------------ parsers
std::vector<Fix> parse_adsb_lol(const std::string& body) {
  std::vector<Fix> out;
  const json j = json::parse(body);
  const double now = j.value("now", 0.0) / 1000.0;
  for (const auto& a : j.value("ac", json::array())) {
    const auto lat = num(a, "lat"), lon = num(a, "lon");
    if (!lat || !lon || !a.contains("hex") || !a["hex"].is_string()) continue;
    Fix f;
    f.id = a["hex"].get<std::string>();
    if (a.contains("flight") && a["flight"].is_string()) f.name = trim(a["flight"].get<std::string>());
    f.lat = *lat, f.lon = *lon;
    f.t = now - num(a, "seen_pos").value_or(0);
    const bool gnd = a.contains("alt_baro") && a["alt_baro"].is_string() && a["alt_baro"] == "ground";
    f.alt = gnd ? std::optional<double>(0) : num(a, "alt_baro");
    f.spd = num(a, "gs");
    f.hdg = num(a, "track");
    if (gnd) f.extra["gnd"] = true;
    if (a.contains("r") && a["r"].is_string()) f.extra["reg"] = a["r"];
    if (a.contains("t") && a["t"].is_string()) f.extra["type"] = a["t"];
    if (auto vr = num(a, "baro_rate")) f.extra["vr"] = *vr;
    out.push_back(std::move(f));
  }
  return out;
}

std::vector<Fix> parse_opensky(const std::string& body) {
  std::vector<Fix> out;
  const json j = json::parse(body);
  const double now = j.value("time", 0.0);
  if (!j.contains("states") || !j["states"].is_array()) return out;
  for (const auto& s : j["states"]) {
    if (!s.is_array() || s.size() < 12 || !s[5].is_number() || !s[6].is_number() || !s[0].is_string()) continue;
    Fix f;
    f.id = s[0].get<std::string>();
    if (s[1].is_string()) f.name = trim(s[1].get<std::string>());
    f.lon = s[5].get<double>(), f.lat = s[6].get<double>();
    f.t = s[3].is_number() ? s[3].get<double>() : now;
    const bool gnd = s[8].is_boolean() && s[8].get<bool>();
    if (gnd) f.alt = 0, f.extra["gnd"] = true;
    else if (s[7].is_number()) f.alt = std::round(s[7].get<double>() * 3.28084);  // metres -> feet
    if (s[9].is_number()) f.spd = s[9].get<double>() * 1.94384;                    // m/s -> knots
    if (s[10].is_number()) f.hdg = s[10].get<double>();
    if (s[11].is_number()) f.extra["vr"] = std::round(s[11].get<double>() * 196.85);  // m/s -> ft/min
    if (s[2].is_string()) f.extra["ctry"] = s[2];
    out.push_back(std::move(f));
  }
  return out;
}

std::vector<Fix> parse_ingest(const json& j) {
  std::vector<Fix> out;
  const auto& arr = j.at("fixes");
  if (!arr.is_array()) throw std::invalid_argument("fixes must be an array");
  if (arr.size() > 50000) throw std::invalid_argument("at most 50000 fixes per post");
  for (const auto& x : arr) {
    if (!x.is_object()) continue;
    const auto lat = num(x, "lat"), lon = num(x, "lon"), t = num(x, "t");
    if (!lat || !lon || !t || !x.contains("id") || *lon < -180 || *lon > 180 || *lat < -90 || *lat > 90) continue;
    Fix f;
    f.id = x["id"].is_string() ? x["id"].get<std::string>() : x["id"].dump();
    if (x.contains("name") && x["name"].is_string()) f.name = trim(x["name"].get<std::string>()).substr(0, 64);
    f.lat = *lat, f.lon = *lon, f.t = *t;
    f.alt = num(x, "alt"), f.spd = num(x, "spd"), f.hdg = num(x, "hdg");
    if (x.contains("extra") && x["extra"].is_object() && x["extra"].dump().size() <= 2048) f.extra = x["extra"];
    out.push_back(std::move(f));
  }
  return out;
}

// ------------------------------------------------------------------ fetch + poller
std::optional<std::string> curl_fetch(const std::string& url) {
  // Only fixed feed URLs come here (no user input), but keep the quoting strict anyway.
  if (url.find_first_of("\"'`$\\ \n\r") != std::string::npos) return std::nullopt;
#ifdef _WIN32
  const std::string cmd = "curl.exe -s -L -m 20 -A gsa-backend -H \"Accept: application/json\" \"" + url + "\" 2>NUL";
  FILE* p = _popen(cmd.c_str(), "rb");
#else
  const std::string cmd = "curl -s -L -m 20 -A gsa-backend -H 'Accept: application/json' '" + url + "' 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
#endif
  if (!p) return std::nullopt;
  std::string body;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
    body.append(buf, n);
    if (body.size() > (size_t(64) << 20)) break;
  }
#ifdef _WIN32
  const int rc = _pclose(p);
#else
  const int rc = pclose(p);
#endif
  if (rc != 0 || body.empty()) return std::nullopt;
  return body;
}

PlanePoller::PlanePoller(std::shared_ptr<Store> store, Fetch fetch, double interval_s)
    : store_(std::move(store)), fetch_(std::move(fetch)), interval_s_(interval_s) {}

PlanePoller::~PlanePoller() { stop(); }

bool PlanePoller::poll_once() {
  // adsb.lol (ODbL) around Johannesburg, Cape Town, Durban and the centre; OpenSky (CC BY 4.0) over SA if none answer.
  static const double hubs[][2] = {{-26.1, 28.2}, {-33.9, 18.6}, {-29.9, 30.9}, {-29.5, 24.5}};
  std::vector<Fix> all;
  int ok = 0;
  std::string err;
  for (const auto& h : hubs) {
    char url[160];
    std::snprintf(url, sizeof url, "https://api.adsb.lol/v2/lat/%.1f/lon/%.1f/dist/250", h[0], h[1]);
    try {
      if (auto body = fetch_(url)) {
        auto fx = parse_adsb_lol(*body);
        all.insert(all.end(), std::make_move_iterator(fx.begin()), std::make_move_iterator(fx.end()));
        ++ok;
      }
    } catch (const std::exception& e) {
      err = std::string("adsb.lol: ") + e.what();
    }
  }
  std::string source = "adsb.lol";
  if (!ok) {
    try {
      if (auto body = fetch_("https://opensky-network.org/api/states/all?lamin=-35.6&lomin=15.5&lamax=-21.4&lomax=33.6")) {
        all = parse_opensky(*body);
        source = "opensky";
        ok = 1;
      } else {
        err = "no plane feed answered";
      }
    } catch (const std::exception& e) {
      err = std::string("opensky: ") + e.what();
    }
  }
  const double now = now_s();
  if (ok) store_->ingest("plane", all, source, now);
  std::lock_guard<std::mutex> lk(mu_);
  if (ok) last_source_ = source, last_ok_ = now, last_error_.clear();
  else last_error_ = err;
  return ok > 0;
}

void PlanePoller::start() {
  std::lock_guard<std::mutex> life(life_);
  if (run_.exchange(true)) return;
  if (th_.joinable()) th_.join();
  th_ = std::thread([this] {
    double next_prune = 0;
    while (run_) {
      const auto t0 = std::chrono::steady_clock::now();
      try {
        poll_once();
        if (now_s() > next_prune) store_->prune(now_s()), next_prune = now_s() + 600;
      } catch (...) {
      }
      // sleep in small steps so stop() returns quickly
      while (run_ && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < interval_s_)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  });
}

void PlanePoller::stop() {
  std::lock_guard<std::mutex> life(life_);
  run_ = false;
  if (th_.joinable()) th_.join();
}

json PlanePoller::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  json j{{"running", run_.load()}, {"interval_s", interval_s_}};
  if (!last_source_.empty()) j["source"] = last_source_;
  if (last_ok_ > 0) j["last_ok"] = std::round(last_ok_);
  if (!last_error_.empty()) j["error"] = last_error_;
  return j;
}

}  // namespace gsa::live
