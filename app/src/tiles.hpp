// Fetches and decodes tiles on worker threads; keeps decoded tiles for the renderer.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "api_client.hpp"
#include "geo.hpp"
#include "mvt.hpp"

namespace gsa {

struct TileKey {
  std::string pack;
  geo::TileId id;
  bool operator<(const TileKey& o) const { return pack != o.pack ? pack < o.pack : id < o.id; }
  bool operator==(const TileKey& o) const = default;
};

// A decoded image tile from a raster pack ("raster-..."), RGBA8.
struct Raster {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
};
bool is_raster_pack(const std::string& pack);

struct LoadedTile {
  TileKey key;
  std::shared_ptr<const mvt::Tile> tile;  // null = the pack has no tile here (204)
  std::shared_ptr<const Raster> raster;   // raster packs only
};

class TileLoader {
 public:
  TileLoader(api::Endpoint ep, int threads = 4);
  ~TileLoader();

  // Ask for tiles, best first. Requests not in the newest `want` list are dropped from the queue.
  void want(const std::vector<TileKey>& keys);
  // Tiles finished since the last call.
  std::vector<LoadedTile> take();

  int in_flight() const { return in_flight_.load(); }
  int errors() const { return errors_.load(); }
  double last_ms() const { return last_ms_.load(); }

 private:
  void run();
  api::Endpoint ep_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<TileKey> queue_;
  std::set<TileKey> pending_;  // queued or being fetched
  std::vector<LoadedTile> done_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stop_{false};
  std::atomic<int> in_flight_{0}, errors_{0};
  std::atomic<double> last_ms_{0};
};

}  // namespace gsa
