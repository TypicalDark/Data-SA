// Draws distance-field text and icons (text::Vertex quads) with the glyph atlas texture.
#pragma once

#include <vector>

#include "text/atlas.hpp"
#include "text/labels.hpp"
#include "vk_context.hpp"

namespace gsa {

class TextRenderer {
 public:
  explicit TextRenderer(vk::Context& ctx);
  ~TextRenderer();
  TextRenderer(const TextRenderer&) = delete;
  TextRenderer& operator=(const TextRenderer&) = delete;

  // Copies the atlas to the GPU when it changed. Call outside the render pass.
  void upload(VkCommandBuffer cmd, text::Atlas& atlas);
  // Draws quads inside the render pass.
  void draw(VkCommandBuffer cmd, const std::vector<text::Vertex>& verts, float dpr);

 private:
  vk::Context& ctx_;
  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory image_mem_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  bool initialised_ = false;  // image has left VK_IMAGE_LAYOUT_UNDEFINED
  vk::Buffer verts_[vk::Context::kFramesInFlight];
  uint64_t slot_ = 0;
};

}  // namespace gsa
