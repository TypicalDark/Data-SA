#include "tiles.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

namespace gsa {

bool is_raster_pack(const std::string& pack) { return pack.rfind("raster-", 0) == 0; }

namespace {
std::shared_ptr<const Raster> decode_png(const std::string& body) {
  int w = 0, h = 0, n = 0;
  unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(body.data()), int(body.size()), &w,
                                            &h, &n, 4);
  if (!px) throw std::runtime_error(std::string("bad image: ") + stbi_failure_reason());
  auto r = std::make_shared<Raster>();
  r->w = w;
  r->h = h;
  r->rgba.assign(px, px + size_t(w) * size_t(h) * 4);
  stbi_image_free(px);
  return r;
}
}  // namespace

TileLoader::TileLoader(api::Endpoint ep, int threads) : ep_(std::move(ep)) {
  for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
}

TileLoader::~TileLoader() {
  stop_ = true;
  cv_.notify_all();
  for (auto& t : threads_) t.join();
}

void TileLoader::want(const std::vector<TileKey>& keys) {
  std::lock_guard<std::mutex> lk(mu_);
  // drop queued requests the view no longer needs; keep the ones already being fetched
  for (const auto& k : queue_) pending_.erase(k);
  queue_.clear();
  for (const auto& k : keys) {
    if (pending_.count(k)) continue;
    queue_.push_back(k);
    pending_.insert(k);
  }
  cv_.notify_all();
}

std::vector<LoadedTile> TileLoader::take() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<LoadedTile> out;
  out.swap(done_);
  return out;
}

void TileLoader::run() {
  api::Client client(ep_);
  for (;;) {
    TileKey k;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      k = queue_.front();
      queue_.pop_front();
    }
    ++in_flight_;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = client.tile(k.pack, k.id.z, k.id.x, k.id.y);
    LoadedTile lt{k, nullptr};
    bool ok = true;
    if (r.status == 200) {
      try {
        if (is_raster_pack(k.pack)) lt.raster = decode_png(r.body);
        else lt.tile = std::make_shared<const mvt::Tile>(mvt::decode(r.body));
      } catch (const std::exception& e) {
        std::fprintf(stderr, "tile %s/%d/%u/%u: %s\n", k.pack.c_str(), k.id.z, k.id.x, k.id.y, e.what());
        ok = false;
      }
    } else if (r.status != 204) {
      ok = false;
      if (errors_ < 5)
        std::fprintf(stderr, "tile %s/%d/%u/%u: %d %s\n", k.pack.c_str(), k.id.z, k.id.x, k.id.y, r.status, r.error.c_str());
    }
    last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    --in_flight_;
    std::lock_guard<std::mutex> lk(mu_);
    pending_.erase(k);
    if (ok) done_.push_back(std::move(lt));
    else ++errors_;
  }
}

}  // namespace gsa
