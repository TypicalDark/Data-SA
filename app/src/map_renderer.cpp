#include "map_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "shaders/map_frag.h"
#include "shaders/map_vert.h"

namespace gsa {

namespace {
constexpr size_t kMaxTiles = 600;

struct Push {
  float origin[2];
  float k;
  float zoom;
  float inv_half[2];
  float alpha;
  float px;
};
static_assert(sizeof(Push) == 32);
}  // namespace

MapRenderer::MapRenderer(vk::Context& ctx) : ctx_(ctx), text_(ctx) {
  const VkDevice dev = ctx.device();
  VkShaderModule vs = ctx.shader(map_vert, sizeof(map_vert)), fs = ctx.shader(map_frag, sizeof(map_frag));

  VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.pushConstantRangeCount = 1;
  li.pPushConstantRanges = &pr;
  vk::check(vkCreatePipelineLayout(dev, &li, nullptr, &layout_), "vkCreatePipelineLayout");

  VkPipelineShaderStageCreateInfo st[2]{};
  st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
  st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
  VkVertexInputBindingDescription bind{0, sizeof(mesh::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attr[4] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(mesh::Vertex, x)},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(mesh::Vertex, ex)},
      {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(mesh::Vertex, rgba)},
      {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(mesh::Vertex, minz)},
  };
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &bind;
  vi.vertexAttributeDescriptionCount = 4;
  vi.pVertexAttributeDescriptions = attr;
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = ctx.samples();
  VkPipelineColorBlendAttachmentState cb{};
  cb.blendEnable = VK_TRUE;
  cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cb.colorBlendOp = VK_BLEND_OP_ADD;
  cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cb.alphaBlendOp = VK_BLEND_OP_ADD;
  cb.colorWriteMask = 0xf;
  VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  bs.attachmentCount = 1;
  bs.pAttachments = &cb;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pi.stageCount = 2;
  pi.pStages = st;
  pi.pVertexInputState = &vi;
  pi.pInputAssemblyState = &ia;
  pi.pViewportState = &vp;
  pi.pRasterizationState = &rs;
  pi.pMultisampleState = &ms;
  pi.pColorBlendState = &bs;
  pi.pDynamicState = &ds;
  pi.layout = layout_;
  pi.renderPass = ctx.render_pass();
  vk::check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_), "vkCreateGraphicsPipelines");
  vkDestroyShaderModule(dev, vs, nullptr);
  vkDestroyShaderModule(dev, fs, nullptr);
}

MapRenderer::~MapRenderer() {
  vkDeviceWaitIdle(ctx_.device());
  for (auto& [k, g] : tiles_) ctx_.destroy_now(g.buf);
  vkDestroyPipeline(ctx_.device(), pipeline_, nullptr);
  vkDestroyPipelineLayout(ctx_.device(), layout_, nullptr);
}

void MapRenderer::upload(const TileKey& k, GpuTile& g) {
  ctx_.destroy_later(g.buf);
  g.buf = {};
  g.fill = g.lines = g.symbols = 0;
  if (!g.tile) return;
  mesh::Mesh m;
  mesh::build(k.pack, *g.tile, style_, m);
  if (m.vertex_count() == 0) return;
  g.buf = ctx_.create_buffer(m.vertex_count() * sizeof(mesh::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  auto* dst = static_cast<mesh::Vertex*>(g.buf.mapped);
  std::memcpy(dst, m.fill.data(), m.fill.size() * sizeof(mesh::Vertex));
  std::memcpy(dst + m.fill.size(), m.lines.data(), m.lines.size() * sizeof(mesh::Vertex));
  std::memcpy(dst + m.fill.size() + m.lines.size(), m.symbols.data(), m.symbols.size() * sizeof(mesh::Vertex));
  g.fill = uint32_t(m.fill.size());
  g.lines = uint32_t(m.lines.size());
  g.symbols = uint32_t(m.symbols.size());
}

void MapRenderer::set_style(const layers::MapStyle& s) {
  if (s == style_) return;
  style_ = s;
  for (auto& [k, g] : tiles_) upload(k, g);
}

void MapRenderer::add(const LoadedTile& t) {
  auto& g = tiles_[t.key];
  g.tile = t.tile;
  g.used = frame_;
  upload(t.key, g);
  if (tiles_.size() > kMaxTiles) {
    // evict the least recently drawn quarter
    std::vector<std::pair<uint64_t, TileKey>> age;
    for (const auto& [k, v] : tiles_) age.emplace_back(v.used, k);
    std::sort(age.begin(), age.end());
    for (size_t i = 0; i < kMaxTiles / 4; ++i) {
      auto it = tiles_.find(age[i].second);
      ctx_.destroy_later(it->second.buf);
      tiles_.erase(it);
    }
  }
}

int MapRenderer::tile_zoom(double zoom, const PackZoom& p) {
  return std::clamp(int(std::floor(zoom + 0.25)), p.minzoom, p.maxzoom);
}

std::vector<TileKey> MapRenderer::needed(const geo::Camera& cam, const std::vector<PackZoom>& packs) const {
  std::vector<TileKey> out;
  for (const auto& p : packs) {
    const int tz = tile_zoom(cam.zoom, p);
    // the parent level first, so something shows quickly when jumping far
    if (tz > p.minzoom)
      for (const auto& id : geo::cover(cam, tz - 1))
        if (!has({p.pack, id})) out.push_back({p.pack, id});
    for (const auto& id : geo::cover(cam, tz))
      if (!has({p.pack, id})) out.push_back({p.pack, id});
  }
  return out;
}

std::vector<TileKey> MapRenderer::drawable(const geo::Camera& cam, const std::vector<PackZoom>& packs) const {
  std::vector<TileKey> out;
  std::set<TileKey> seen;
  for (const auto& p : packs) {
    const int tz = tile_zoom(cam.zoom, p);
    std::vector<TileKey> fallbacks, exact;
    for (const auto& id : geo::cover(cam, tz)) {
      TileKey k{p.pack, id};
      if (has(k)) {
        if (seen.insert(k).second) exact.push_back(k);
        continue;
      }
      // nearest loaded ancestor stands in until the tile arrives
      for (auto a = id; a.z > p.minzoom;) {
        a = a.parent();
        TileKey ak{p.pack, a};
        if (has(ak)) {
          if (seen.insert(ak).second) fallbacks.push_back(ak);
          break;
        }
      }
    }
    std::sort(fallbacks.begin(), fallbacks.end(), [](const auto& a, const auto& b) { return a.id.z < b.id.z; });
    out.insert(out.end(), fallbacks.begin(), fallbacks.end());
    out.insert(out.end(), exact.begin(), exact.end());
  }
  return out;
}

bool MapRenderer::load_fonts(const std::string& dir) {
  fonts_.regular = atlas_.add_font(dir + "/fonts/SpaceGrotesk-Regular.ttf");
  fonts_.medium = atlas_.add_font(dir + "/fonts/SpaceGrotesk-Medium.ttf");
  fonts_.mono = atlas_.add_font(dir + "/fonts/IBMPlexMono-Regular.ttf");
  const bool ok = fonts_.regular >= 0 && fonts_.medium >= 0 && fonts_.mono >= 0;
  // fall back to whichever face loaded
  const int any = std::max({fonts_.regular, fonts_.medium, fonts_.mono});
  for (int* f : {&fonts_.regular, &fonts_.medium, &fonts_.mono})
    if (*f < 0) *f = any;
  return ok;
}

void MapRenderer::prepare(const geo::Camera& cam, const std::vector<PackZoom>& packs,
                          const std::vector<text::Rect>& blockers, double dt) {
  keys_ = drawable(cam, packs);
  text_verts_.clear();
  if (atlas_.font_count() == 0) return;
  const int generation = atlas_.generation();
  for (int attempt = 0; attempt < 2; ++attempt) {
    engine_.begin(float(cam.w), float(cam.h), blockers);
    picks_.clear();
    const double scale = cam.scale();
    for (const auto& k : keys_) {
      const auto& g = tiles_.at(k);
      if (!g.tile) continue;
      map_labels::TileOnScreen t;
      t.tile = g.tile;
      t.picks = &picks_;
      t.pack = k.pack;
      t.id = k.id;
      cam.to_screen(k.id.ox(), k.id.oy(), t.ox, t.oy);
      t.k = k.id.size() * scale;
      map_labels::collect(t, style_, cam.zoom, fonts_, engine_);
    }
    engine_.finish(dt, text_verts_);
    // the atlas filled up and was cleared mid-frame: lay out once more against the new atlas
    if (atlas_.generation() == generation) break;
  }
}

void MapRenderer::upload(VkCommandBuffer cmd) { text_.upload(cmd, atlas_); }

void MapRenderer::draw(VkCommandBuffer cmd, const geo::Camera& cam, float dpr) {
  ++frame_;
  const auto& keys = keys_;
  vertices_ = 0;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  const VkExtent2D ext = ctx_.extent();
  Push pc{};
  pc.zoom = float(cam.zoom);
  pc.inv_half[0] = 2.0f / float(ext.width);
  pc.inv_half[1] = 2.0f / float(ext.height);
  pc.alpha = 1.0f;
  pc.px = dpr;
  const double scale = cam.scale();

  // three passes so borders and symbols always sit above every tile's fills
  for (int pass = 0; pass < 3; ++pass) {
    for (const auto& k : keys) {
      auto it = tiles_.find(k);
      if (it == tiles_.end()) continue;
      auto& g = it->second;
      g.used = frame_;
      if (!g.buf.buf) continue;
      const uint32_t first = pass == 0 ? 0 : pass == 1 ? g.fill : g.fill + g.lines;
      const uint32_t count = pass == 0 ? g.fill : pass == 1 ? g.lines : g.symbols;
      if (!count) continue;
      double sx, sy;
      cam.to_screen(k.id.ox(), k.id.oy(), sx, sy);
      pc.origin[0] = float(sx * dpr);
      pc.origin[1] = float(sy * dpr);
      pc.k = float(k.id.size() * scale * dpr);
      vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push), &pc);
      const VkDeviceSize off = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &g.buf.buf, &off);
      vkCmdDraw(cmd, count, 1, first, 0);
      vertices_ += count;
    }
  }
  text_.draw(cmd, text_verts_, dpr);
  vertices_ += text_verts_.size();
}

const map_labels::FeatureRef* MapRenderer::pick_icon(double sx, double sy) const {
  const int i = engine_.hit(float(sx), float(sy));
  return i >= 0 && size_t(i) < picks_.size() ? &picks_[size_t(i)] : nullptr;
}

const mvt::Feature* MapRenderer::pick(double wx, double wy, const std::string& layer, double zoom) const {
  // search from the most detailed loaded tile down
  for (int z = std::min(14, int(std::floor(zoom + 0.25))); z >= 0; --z) {
    const double n = double(1u << z);
    if (wx < 0 || wy < 0 || wx >= 1 || wy >= 1) return nullptr;
    const geo::TileId id{z, uint32_t(wx * n), uint32_t(wy * n)};
    auto it = tiles_.find({"admin", id});
    if (it == tiles_.end() || !it->second.tile) continue;
    const auto* l = it->second.tile->layer(layer);
    if (!l) return nullptr;
    const float tx = float(wx * n - id.x), ty = float(wy * n - id.y);
    for (const auto& f : l->features)
      if (f.type == mvt::GeomType::Polygon && mvt::point_in_polygon(f, tx, ty)) return &f;
    return nullptr;
  }
  return nullptr;
}

}  // namespace gsa
