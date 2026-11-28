// Live planes and ships: the latest position of everything seen, plus a position history.
//
// Every fix that arrives (from the built-in plane poller or a feeder posting to /v1/live/ingest)
// updates the latest position in memory. A thinned copy goes into a history table in
// <data>/cache/live.sqlite: a new history point when the object moved min_move_m or min_gap_s
// passed. History is kept keep_s (default 24 h) for everything, enough for trails, and
// keep_watched_s (default a year) for the planes and ships on the watch list.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "gsa/sqlite.hpp"

namespace gsa::live {

// One position report. id: ICAO hex for planes, MMSI for ships.
struct Fix {
  std::string id;
  std::string name;  // call sign / flight for planes, ship name for ships
  double t = 0;      // unix seconds of the position
  double lon = 0, lat = 0;
  std::optional<double> alt;  // feet (planes)
  std::optional<double> spd;  // knots over ground
  std::optional<double> hdg;  // degrees, track over ground
  nlohmann::json extra = nlohmann::json::object();  // reg, type, ground, dest, ...
};

struct Bbox {
  double minlon = -180, minlat = -90, maxlon = 180, maxlat = 90;
  bool contains(double lon, double lat) const { return lon >= minlon && lon <= maxlon && lat >= minlat && lat <= maxlat; }
};

struct Options {
  std::filesystem::path db;               // empty: history in memory only (tests)
  int64_t keep_s = 24 * 3600;             // history of everything
  int64_t keep_watched_s = 365 * 24 * 3600;  // history of watched objects
  double min_move_m = 50;                 // thinning: store a history point after this much movement
  int64_t min_gap_s = 60;                 // ... or after this many seconds
  int64_t stale_s = 15 * 60;              // drop from the latest map after this long without a fix
};

bool valid_kind(const std::string& kind);  // "plane" or "ship"

class Store {
 public:
  explicit Store(Options opt);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Adds fixes from one source. Returns how many went into the history.
  size_t ingest(const std::string& kind, const std::vector<Fix>& fixes, const std::string& source, double now);

  // Latest positions: {t, kind, sources, n, items:[{id, name, lon, lat, alt, spd, hdg, t, watched, ...extra}]}
  nlohmann::json latest(const std::string& kind, const std::optional<Bbox>& box, double max_age_s, double now) const;
  // Change counter per kind (bumped on every ingest that changed something) and the time of that change.
  uint64_t version(const std::string& kind) const;
  double modified(const std::string& kind) const;

  // History of one object, oldest first: {kind, id, name, watched, n, fixes:[[t, lon, lat, alt, spd, hdg]]}
  nlohmann::json track(const std::string& kind, const std::string& id, double since, double until, size_t limit) const;

  // Watch list: these keep their history for keep_watched_s.
  nlohmann::json watch_list() const;
  bool watch(const std::string& kind, const std::string& id, const std::string& label, double now);  // false if already watched
  bool unwatch(const std::string& kind, const std::string& id);
  bool watched(const std::string& kind, const std::string& id) const;

  // Deletes history past its keep time and stale latest entries. Returns history rows removed.
  size_t prune(double now);

  nlohmann::json status() const;

 private:
  struct Item {
    Fix fix;
    std::string source;
    double hist_t = -1e18, hist_lon = 0, hist_lat = 0;  // last history point written
  };
  struct Kind {
    std::map<std::string, Item> items;
    std::map<std::string, double> sources;  // source -> last ingest time
    uint64_t version = 0;
    double modified = 0;
  };
  Options opt_;
  mutable std::mutex mu_;
  std::map<std::string, Kind> kinds_;
  std::map<std::pair<std::string, std::string>, nlohmann::json> watch_;
  std::unique_ptr<sql::Db> db_;
  uint64_t hist_rows_ = 0;
};

// Parsers for the plane feeds (exposed for tests).
std::vector<Fix> parse_adsb_lol(const std::string& body);   // api.adsb.lol /v2/lat/../lon/../dist/..
std::vector<Fix> parse_opensky(const std::string& body);    // opensky-network.org /api/states/all
// Feeder body for POST /v1/live/ingest: {kind, source, fixes:[{id, name, t, lon, lat, alt, spd, hdg, extra}]}
std::vector<Fix> parse_ingest(const nlohmann::json& j);

// Fetches a URL and returns the body, or nullopt. The default runs the system curl (Windows 10+
// ships curl.exe), so the back-end needs no TLS library and honours HTTPS_PROXY.
using Fetch = std::function<std::optional<std::string>(const std::string& url)>;
std::optional<std::string> curl_fetch(const std::string& url);

// Polls the plane feeds every interval_s: adsb.lol around four SA hubs, OpenSky over SA if that fails.
class PlanePoller {
 public:
  PlanePoller(std::shared_ptr<Store> store, Fetch fetch, double interval_s = 20);
  ~PlanePoller();
  void start();
  void stop();
  bool poll_once();  // one round; true when a source answered
  nlohmann::json status() const;

 private:
  std::shared_ptr<Store> store_;
  Fetch fetch_;
  double interval_s_;
  std::atomic<bool> run_{false};
  std::mutex life_;  // serialises start() and stop()
  std::thread th_;
  mutable std::mutex mu_;
  std::string last_source_, last_error_;
  double last_ok_ = 0;
};

double now_s();

}  // namespace gsa::live
