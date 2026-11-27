// Draws vector tiles with one Vulkan pipeline (fills, lines and symbols share a vertex format).
#pragma once

#include <map>
#include <memory>
#include <vector>

#include "geo.hpp"
#include "mesh.hpp"
#include "style.hpp"
#include "tiles.hpp"
#include "vk_context.hpp"

namespace gsa {

struct PackZoom {
  std::string pack;
  int minzoom = 0, maxzoom = 14;
};

class MapRenderer {
 public:
  explicit MapRenderer(vk::Context& ctx);
  ~MapRenderer();

  void set_topic(style::Topic t);
  void add(const LoadedTile& t);
  bool has(const TileKey& k) const { return tiles_.count(k) != 0; }

  // Tiles the view needs (for the loader), best first.
  std::vector<TileKey> needed(const geo::Camera& cam, const std::vector<PackZoom>& packs) const;
  void draw(VkCommandBuffer cmd, const geo::Camera& cam, float dpr, const std::vector<PackZoom>& packs);

  // Labels from the admin tiles currently drawn.
  const std::vector<mesh::Label>& labels() const { return labels_; }
  // Municipality / province feature under a world point, from loaded admin tiles.
  const mvt::Feature* pick(double wx, double wy, const std::string& layer, double zoom) const;
  size_t tile_count() const { return tiles_.size(); }
  size_t vertex_count() const { return vertices_; }

 private:
  struct GpuTile {
    std::shared_ptr<const mvt::Tile> tile;
    vk::Buffer buf;
    uint32_t fill = 0, lines = 0, symbols = 0;
    uint64_t used = 0;
  };
  void upload(const TileKey& k, GpuTile& g);
  std::vector<TileKey> drawable(const geo::Camera& cam, const std::vector<PackZoom>& packs) const;
  static int tile_zoom(double zoom, const PackZoom& p);

  vk::Context& ctx_;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  style::Topic topic_ = style::Topic::Overview;
  std::map<TileKey, GpuTile> tiles_;
  std::vector<mesh::Label> labels_;
  size_t vertices_ = 0;
  uint64_t frame_ = 0;
};

}  // namespace gsa
