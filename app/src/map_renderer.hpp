// Draws vector tiles with one Vulkan pipeline (fills, lines and symbols share a vertex format).
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "geo.hpp"
#include "map_labels.hpp"
#include "mesh.hpp"
#include "text/atlas.hpp"
#include "text/labels.hpp"
#include "text_renderer.hpp"
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

  void set_style(const layers::MapStyle& s);
  const layers::MapStyle& map_style() const { return style_; }
  void add(const LoadedTile& t);
  bool has(const TileKey& k) const { return tiles_.count(k) != 0; }

  // Tiles the view needs (for the loader), best first.
  std::vector<TileKey> needed(const geo::Camera& cam, const std::vector<PackZoom>& packs) const;

  // Loads the label fonts from the assets folder; returns false when one is missing.
  bool load_fonts(const std::string& assets_dir);
  // Before the frame: picks the tiles to draw and places labels clear of `blockers` (interface
  // rectangles, logical pixels). dt < 0 skips label fades.
  void prepare(const geo::Camera& cam, const std::vector<PackZoom>& packs, const std::vector<text::Rect>& blockers,
               double dt);
  // Outside the render pass: uploads the glyph atlas when it changed, and new image tiles.
  void upload(VkCommandBuffer cmd);
  // Inside the render pass: fills, image layers, lines, then icons and text.
  void draw(VkCommandBuffer cmd, const geo::Camera& cam, float dpr);

  bool labels_animating() const { return engine_.animating(); }
  size_t labels_placed() const { return engine_.placed(); }
  // The facility icon under a screen point (logical pixels), from the last prepare().
  const map_labels::FeatureRef* pick_icon(double sx, double sy) const;
  // Facilities drawn in the last frame (for lists and ranking).
  const std::vector<map_labels::FeatureRef>& drawn_facilities() const { return picks_; }
  // Municipality / province feature under a world point, from loaded admin tiles.
  const mvt::Feature* pick(double wx, double wy, const std::string& layer, double zoom) const;
  // The top website-layer feature under a screen point (points and lines within a few pixels).
  struct WebHit {
    const mvt::Feature* feature = nullptr;
    std::string layer;
    double wx = 0, wy = 0;  // world position of the feature (point) or of the click
  };
  std::optional<WebHit> pick_web(double sx, double sy, const geo::Camera& cam) const;
  size_t tile_count() const { return tiles_.size(); }
  size_t vertex_count() const { return vertices_; }

 private:
  struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };
  struct GpuTile {
    std::shared_ptr<const mvt::Tile> tile;
    std::shared_ptr<const Raster> raster;  // image packs: kept until uploaded
    vk::Buffer buf;
    GpuImage img;
    bool ready = false;  // image uploaded
    uint32_t fill = 0, lines = 0, symbols = 0;
    uint64_t used = 0;
  };
  // One image tile, or the matching part of a loaded ancestor, on one screen tile.
  struct RasterDraw {
    TileKey src;
    geo::TileId at;
    float u0 = 0, v0 = 0, uk = 1;
    float alpha = 1;
  };
  void upload(const TileKey& k, GpuTile& g);
  void upload_image(VkCommandBuffer cmd, GpuTile& g);
  void release(GpuTile& g);
  void init_raster_pipeline();
  void draw_rasters(VkCommandBuffer cmd, const geo::Camera& cam, float dpr);
  std::vector<TileKey> drawable(const geo::Camera& cam, const std::vector<PackZoom>& packs) const;
  static int tile_zoom(double zoom, const PackZoom& p);

  vk::Context& ctx_;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout raster_layout_ = VK_NULL_HANDLE;
  VkPipeline raster_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout raster_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool raster_pool_ = VK_NULL_HANDLE;
  VkSampler raster_sampler_ = VK_NULL_HANDLE;
  std::vector<RasterDraw> raster_draws_;  // from prepare()
  layers::MapStyle style_;
  std::vector<map_labels::FeatureRef> picks_;
  std::map<TileKey, GpuTile> tiles_;
  std::vector<TileKey> keys_;  // drawn this frame, from prepare()
  text::Atlas atlas_;
  text::Engine engine_{atlas_};
  TextRenderer text_;
  map_labels::Fonts fonts_;
  std::vector<text::Vertex> text_verts_;
  size_t vertices_ = 0;
  uint64_t frame_ = 0;
};

}  // namespace gsa
