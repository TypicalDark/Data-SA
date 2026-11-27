#include "text_renderer.hpp"

#include <cstring>

#include "shaders/text_frag.h"
#include "shaders/text_vert.h"

namespace gsa {

namespace {
struct Push {
  float inv_half[2];
  float px;
  float on_edge;
  float unit;
  float tex_size;
  float pad[2];
};
static_assert(sizeof(Push) == 32);
constexpr VkDeviceSize kAtlasBytes = VkDeviceSize(text::Atlas::kSize) * text::Atlas::kSize;
}  // namespace

TextRenderer::TextRenderer(vk::Context& ctx) : ctx_(ctx) {
  const VkDevice dev = ctx.device();

  // atlas texture
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8_UNORM;
  ii.extent = {uint32_t(text::Atlas::kSize), uint32_t(text::Atlas::kSize), 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vk::check(vkCreateImage(dev, &ii, nullptr, &image_), "vkCreateImage(atlas)");
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, image_, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = ctx.memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vk::check(vkAllocateMemory(dev, &ai, nullptr, &image_mem_), "vkAllocateMemory(atlas)");
  vkBindImageMemory(dev, image_, image_mem_, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = image_;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk::check(vkCreateImageView(dev, &vi, nullptr, &view_), "vkCreateImageView(atlas)");
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = 0;
  vk::check(vkCreateSampler(dev, &si, nullptr, &sampler_), "vkCreateSampler");

  // descriptor set: binding 0 = atlas
  VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  sli.bindingCount = 1;
  sli.pBindings = &b;
  vk::check(vkCreateDescriptorSetLayout(dev, &sli, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = 1;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = &ps;
  vk::check(vkCreateDescriptorPool(dev, &pi, nullptr, &pool_), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = pool_;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &set_layout_;
  vk::check(vkAllocateDescriptorSets(dev, &dai, &set_), "vkAllocateDescriptorSets");
  VkDescriptorImageInfo dii{sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set_;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &dii;
  vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

  // pipeline
  VkShaderModule vs = ctx.shader(text_vert, sizeof(text_vert)), fs = ctx.shader(text_frag, sizeof(text_frag));
  VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.setLayoutCount = 1;
  li.pSetLayouts = &set_layout_;
  li.pushConstantRangeCount = 1;
  li.pPushConstantRanges = &pr;
  vk::check(vkCreatePipelineLayout(dev, &li, nullptr, &layout_), "vkCreatePipelineLayout(text)");

  VkPipelineShaderStageCreateInfo st[2]{};
  st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
  st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
  VkVertexInputBindingDescription bind{0, sizeof(text::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attr[5] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(text::Vertex, x)},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(text::Vertex, u)},
      {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(text::Vertex, fill)},
      {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(text::Vertex, halo)},
      {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(text::Vertex, halo_w)},
  };
  VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vin.vertexBindingDescriptionCount = 1;
  vin.pVertexBindingDescriptions = &bind;
  vin.vertexAttributeDescriptionCount = 5;
  vin.pVertexAttributeDescriptions = attr;
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
  gpi.layout = layout_;
  gpi.renderPass = ctx.render_pass();
  vk::check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_), "vkCreateGraphicsPipelines(text)");
  vkDestroyShaderModule(dev, vs, nullptr);
  vkDestroyShaderModule(dev, fs, nullptr);
}

TextRenderer::~TextRenderer() {
  const VkDevice dev = ctx_.device();
  vkDeviceWaitIdle(dev);
  for (auto& b : verts_) ctx_.destroy_now(b);
  vkDestroyPipeline(dev, pipeline_, nullptr);
  vkDestroyPipelineLayout(dev, layout_, nullptr);
  vkDestroyDescriptorPool(dev, pool_, nullptr);
  vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
  vkDestroySampler(dev, sampler_, nullptr);
  vkDestroyImageView(dev, view_, nullptr);
  vkDestroyImage(dev, image_, nullptr);
  vkFreeMemory(dev, image_mem_, nullptr);
}

void TextRenderer::upload(VkCommandBuffer cmd, text::Atlas& atlas) {
  if (!atlas.dirty() && initialised_) return;
  vk::Buffer staging = ctx_.create_buffer(kAtlasBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  std::memcpy(staging.mapped, atlas.pixels().data(), size_t(kAtlasBytes));

  // Earlier frames may still sample the atlas; a barrier orders this copy after them.
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = initialised_ ? VK_ACCESS_SHADER_READ_BIT : 0;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = initialised_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image_;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, initialised_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  VkBufferImageCopy c{};
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.imageExtent = {uint32_t(text::Atlas::kSize), uint32_t(text::Atlas::kSize), 1};
  vkCmdCopyBufferToImage(cmd, staging.buf, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
  ctx_.destroy_later(staging);
  initialised_ = true;
  atlas.clear_dirty();
}

void TextRenderer::draw(VkCommandBuffer cmd, const std::vector<text::Vertex>& verts, float dpr) {
  if (verts.empty() || !initialised_) return;
  auto& buf = verts_[slot_++ % vk::Context::kFramesInFlight];
  const VkDeviceSize bytes = verts.size() * sizeof(text::Vertex);
  if (buf.size < bytes) {
    ctx_.destroy_later(buf);
    buf = ctx_.create_buffer(bytes + bytes / 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  }
  std::memcpy(buf.mapped, verts.data(), size_t(bytes));
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
  const VkExtent2D ext = ctx_.extent();
  Push pc{{2.0f / float(ext.width), 2.0f / float(ext.height)}, dpr, float(text::Atlas::kOnEdge) / 255.0f,
          text::Atlas::kDistScale / 255.0f, float(text::Atlas::kSize), {0, 0}};
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &pc);
  const VkDeviceSize off = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &buf.buf, &off);
  vkCmdDraw(cmd, uint32_t(verts.size()), 1, 0, 0);
}

}  // namespace gsa
