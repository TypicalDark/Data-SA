#include "map_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "shaders/map_frag.h"
#include "shaders/map_vert.h"
#include "shaders/raster_frag.h"
#include "shaders/raster_vert.h"

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

struct RasterPush {
  float origin[2];
  float k;
  float alpha;
  float inv_half[2];
  float uv0[2];
  float uvk;
  float pad[3];
};
static_assert(sizeof(RasterPush) == 48);
constexpr uint32_t kMaxImages = 512;      // descriptor sets for image tiles
constexpr int kImageUploadsPerFrame = 12;  // spread big jumps over a few frames
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
  init_raster_pipeline();
}

void MapRenderer::init_raster_pipeline() {
  const VkDevice dev = ctx_.device();
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = 0;
  vk::check(vkCreateSampler(dev, &si, nullptr, &raster_sampler_), "vkCreateSampler(raster)");

  VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  sli.bindingCount = 1;
  sli.pBindings = &b;
  vk::check(vkCreateDescriptorSetLayout(dev, &sli, nullptr, &raster_set_layout_), "vkCreateDescriptorSetLayout(raster)");
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxImages};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pi.maxSets = kMaxImages;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = &ps;
  vk::check(vkCreateDescriptorPool(dev, &pi, nullptr, &raster_pool_), "vkCreateDescriptorPool(raster)");

  VkShaderModule vs = ctx_.shader(raster_vert, sizeof(raster_vert)), fs = ctx_.shader(raster_frag, sizeof(raster_frag));
  VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RasterPush)};
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.setLayoutCount = 1;
  li.pSetLayouts = &raster_set_layout_;
  li.pushConstantRangeCount = 1;
  li.pPushConstantRanges = &pr;
  vk::check(vkCreatePipelineLayout(dev, &li, nullptr, &raster_layout_), "vkCreatePipelineLayout(raster)");

  VkPipelineShaderStageCreateInfo st[2]{};
  st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
  st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
  VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
  ms.rasterizationSamples = ctx_.samples();
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
  VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpi.stageCount = 2;
  gpi.pStages = st;
  gpi.pVertexInputState = &vin;
  gpi.pInputAssemblyState = &ia;
  gpi.pViewportState = &vp;
  gpi.pRasterizationState = &rs;
  gpi.pMultisampleState = &ms;
  gpi.pColorBlendState = &bs;
  gpi.pDynamicState = &ds;
  gpi.layout = raster_layout_;
  gpi.renderPass = ctx_.render_pass();
  vk::check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &raster_pipeline_),
            "vkCreateGraphicsPipelines(raster)");
  vkDestroyShaderModule(dev, vs, nullptr);
  vkDestroyShaderModule(dev, fs, nullptr);
}

MapRenderer::~MapRenderer() {
  const VkDevice dev = ctx_.device();
  vkDeviceWaitIdle(dev);
  for (auto& [k, g] : tiles_) {
    ctx_.destroy_now(g.buf);
    release(g);
  }
  ctx_.flush_garbage();  // image tiles queued for release free their sets from the pool below
  vkDestroyPipeline(dev, raster_pipeline_, nullptr);
  vkDestroyPipelineLayout(dev, raster_layout_, nullptr);
  vkDestroyDescriptorPool(dev, raster_pool_, nullptr);
  vkDestroyDescriptorSetLayout(dev, raster_set_layout_, nullptr);
  vkDestroySampler(dev, raster_sampler_, nullptr);
  vkDestroyPipeline(dev, pipeline_, nullptr);
  vkDestroyPipelineLayout(dev, layout_, nullptr);
}

void MapRenderer::release(GpuTile& g) {
  if (!g.img.image) return;
  const VkDevice dev = ctx_.device();
  const GpuImage img = g.img;
  const VkDescriptorPool pool = raster_pool_;
  ctx_.destroy_later([dev, img, pool] {
    if (img.set) vkFreeDescriptorSets(dev, pool, 1, &img.set);
    vkDestroyImageView(dev, img.view, nullptr);
    vkDestroyImage(dev, img.image, nullptr);
    vkFreeMemory(dev, img.mem, nullptr);
  });
  g.img = {};
  g.ready = false;
}

void MapRenderer::upload_image(VkCommandBuffer cmd, GpuTile& g) {
  const VkDevice dev = ctx_.device();
  const Raster& r = *g.raster;
  const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = raster_pool_;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &raster_set_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(dev, &dai, &set) != VK_SUCCESS) return;  // pool full: try again after eviction

  GpuImage& im = g.img;
  im.set = set;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = fmt;
  ii.extent = {uint32_t(r.w), uint32_t(r.h), 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vk::check(vkCreateImage(dev, &ii, nullptr, &im.image), "vkCreateImage(tile)");
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, im.image, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = ctx_.memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vk::check(vkAllocateMemory(dev, &ai, nullptr, &im.mem), "vkAllocateMemory(tile)");
  vkBindImageMemory(dev, im.image, im.mem, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = im.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk::check(vkCreateImageView(dev, &vi, nullptr, &im.view), "vkCreateImageView(tile)");
  VkDescriptorImageInfo dii{raster_sampler_, im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &dii;
  vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

  vk::Buffer staging = ctx_.create_buffer(r.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  std::memcpy(staging.mapped, r.rgba.data(), r.rgba.size());
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &b);
  VkBufferImageCopy c{};
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.imageExtent = {uint32_t(r.w), uint32_t(r.h), 1};
  vkCmdCopyBufferToImage(cmd, staging.buf, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
  ctx_.destroy_later(staging);
  g.ready = true;
  g.raster.reset();  // the pixels live on the GPU now
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
  if (is_raster_pack(t.key.pack)) {
    release(g);
    g.raster = t.raster;
  } else upload(t.key, g);
  if (tiles_.size() > kMaxTiles) {
    // evict the least recently drawn quarter
    std::vector<std::pair<uint64_t, TileKey>> age;
    for (const auto& [k, v] : tiles_) age.emplace_back(v.used, k);
    std::sort(age.begin(), age.end());
    for (size_t i = 0; i < kMaxTiles / 4; ++i) {
      auto it = tiles_.find(age[i].second);
      ctx_.destroy_later(it->second.buf);
      release(it->second);
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
  // image layers: every screen tile shows its own image, or the matching part of the nearest loaded ancestor
  raster_draws_.clear();
  for (const auto& rp : style_.rasters) {
    auto p = std::find_if(packs.begin(), packs.end(), [&](const PackZoom& z) { return z.pack == rp; });
    if (p == packs.end()) continue;
    const float alpha = layers::raster_opacity(rp, cam.zoom);
    if (alpha <= 0.01f) continue;
    const int tz = tile_zoom(cam.zoom, *p);
    for (const auto& id : geo::cover(cam, tz)) {
      for (auto a = id;; a = a.parent()) {
        auto it = tiles_.find({rp, a});
        if (it != tiles_.end() && (it->second.ready || it->second.raster)) {
          RasterDraw d{{rp, a}, id, 0, 0, 1, alpha};
          const int dz = id.z - a.z;
          d.uk = 1.0f / float(1u << dz);
          d.u0 = float(id.x - (a.x << dz)) * d.uk;
          d.v0 = float(id.y - (a.y << dz)) * d.uk;
          raster_draws_.push_back(d);
          it->second.used = frame_;
          break;
        }
        if (it != tiles_.end()) break;  // the pack has nothing here
        if (a.z <= p->minzoom) break;
      }
    }
  }
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

void MapRenderer::upload(VkCommandBuffer cmd) {
  text_.upload(cmd, atlas_);
  int n = 0;
  for (const auto& d : raster_draws_) {
    auto& g = tiles_.at(d.src);
    if (g.ready || !g.raster || g.raster->w <= 0) continue;
    upload_image(cmd, g);
    if (++n >= kImageUploadsPerFrame) break;
  }
}

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

  // three passes so borders and symbols always sit above every tile's fills; images go over the fills
  for (int pass = 0; pass < 3; ++pass) {
    if (pass == 1 && !raster_draws_.empty()) {
      draw_rasters(cmd, cam, dpr);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    }
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

void MapRenderer::draw_rasters(VkCommandBuffer cmd, const geo::Camera& cam, float dpr) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, raster_pipeline_);
  const VkExtent2D ext = ctx_.extent();
  const double scale = cam.scale();
  RasterPush pc{};
  pc.inv_half[0] = 2.0f / float(ext.width);
  pc.inv_half[1] = 2.0f / float(ext.height);
  for (const auto& d : raster_draws_) {
    const auto& g = tiles_.at(d.src);
    if (!g.ready) continue;
    double sx, sy;
    cam.to_screen(d.at.ox(), d.at.oy(), sx, sy);
    pc.origin[0] = float(sx * dpr);
    pc.origin[1] = float(sy * dpr);
    pc.k = float(d.at.size() * scale * dpr);
    pc.alpha = d.alpha;
    pc.uv0[0] = d.u0;
    pc.uv0[1] = d.v0;
    pc.uvk = d.uk;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, raster_layout_, 0, 1, &g.img.set, 0, nullptr);
    vkCmdPushConstants(cmd, raster_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(RasterPush), &pc);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vertices_ += 6;
  }
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

std::optional<MapRenderer::WebHit> MapRenderer::pick_web(double sx, double sy, const geo::Camera& cam) const {
  double wx, wy;
  cam.to_world(sx, sy, wx, wy);
  if (wx < 0 || wy < 0 || wx >= 1 || wy >= 1) return std::nullopt;
  const double scale = cam.scale();
  for (auto e = style_.extra.rbegin(); e != style_.extra.rend(); ++e) {
    // the most detailed drawn tile that holds the point
    const TileKey* best = nullptr;
    for (const auto& k : keys_) {
      if (k.pack.rfind("web-", 0) != 0) continue;
      const double n = double(1u << k.id.z);
      if (uint32_t(wx * n) != k.id.x || uint32_t(wy * n) != k.id.y) continue;
      auto it = tiles_.find(k);
      if (it == tiles_.end() || !it->second.tile || !it->second.tile->layer(e->layer)) continue;
      if (!best || k.id.z > best->id.z) best = &k;
    }
    if (!best) continue;
    const auto& tile = *tiles_.at(*best).tile;
    const double n = double(1u << best->id.z);
    const float tx = float(wx * n - best->id.x), ty = float(wy * n - best->id.y);
    const float tol = float(7.0 / (best->id.size() * scale));
    const mvt::Feature* hit = nullptr;
    float best_d = tol;
    double hx = wx, hy = wy;
    for (const auto& f : tile.layer(e->layer)->features) {
      if (f.type == mvt::GeomType::Polygon) {
        if (!hit && mvt::point_in_polygon(f, tx, ty)) hit = &f;
      } else if (f.type == mvt::GeomType::Point) {
        for (const auto& part : f.parts)
          for (const auto& p : part) {
            const float d = std::hypot(p.x - tx, p.y - ty);
            if (d < best_d) {
              best_d = d;
              hit = &f;
              hx = (best->id.x + double(p.x)) / n;
              hy = (best->id.y + double(p.y)) / n;
            }
          }
      } else if (f.type == mvt::GeomType::Line) {
        for (const auto& part : f.parts)
          for (size_t i = 0; i + 1 < part.size(); ++i) {
            const float ax = part[i].x, ay = part[i].y, bx = part[i + 1].x, by = part[i + 1].y;
            const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
            const float t = l2 > 0 ? std::clamp(((tx - ax) * dx + (ty - ay) * dy) / l2, 0.0f, 1.0f) : 0.0f;
            const float d = std::hypot(ax + t * dx - tx, ay + t * dy - ty);
            if (d < best_d) {
              best_d = d;
              hit = &f;
            }
          }
      }
    }
    if (hit) return WebHit{hit, e->layer, hx, hy};
  }
  return std::nullopt;
}

}  // namespace gsa
