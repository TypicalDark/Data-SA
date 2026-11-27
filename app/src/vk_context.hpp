// Vulkan setup: instance, device, swapchain, MSAA render pass, frames in flight.
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace gsa::vk {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void check(VkResult r, const char* what);

struct Buffer {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void* mapped = nullptr;
};

struct Frame {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkSemaphore acquired = VK_NULL_HANDLE;
  VkFence done = VK_NULL_HANDLE;
};

class Context {
 public:
  static constexpr int kFramesInFlight = 2;

  Context(GLFWwindow* win, bool validation);
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  // Starts a frame: waits for the frame slot, acquires an image, begins the command
  // buffer and the render pass (cleared to `clear`). Returns false when the swapchain
  // had to be rebuilt and the frame should be skipped.
  // `before_pass` records work that must sit outside the render pass (texture uploads).
  bool begin_frame(const float clear[4], const std::function<void(VkCommandBuffer)>& before_pass = {});
  // Ends the render pass, optionally copies the image for a screenshot, submits and presents.
  void end_frame(std::vector<uint8_t>* screenshot_rgba = nullptr);

  VkCommandBuffer cmd() const { return frames_[frame_].cmd; }
  uint64_t frame_number() const { return frame_count_; }
  VkExtent2D extent() const { return extent_; }
  VkRenderPass render_pass() const { return render_pass_; }
  VkSampleCountFlagBits samples() const { return samples_; }
  VkDevice device() const { return device_; }
  VkPhysicalDevice physical() const { return phys_; }
  VkInstance instance() const { return instance_; }
  VkQueue queue() const { return queue_; }
  uint32_t queue_family() const { return qfam_; }
  uint32_t image_count() const { return uint32_t(images_.size()); }
  const std::string& device_name() const { return device_name_; }

  Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage);  // host visible, coherent, mapped
  // Frees after the GPU can no longer be using it (kFramesInFlight frames later).
  void destroy_later(Buffer b);
  void destroy_now(Buffer& b);
  VkShaderModule shader(const uint32_t* spirv, size_t bytes);

  void request_rebuild() { rebuild_ = true; }
  uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags props) const;

 private:
  void create_swapchain();
  void destroy_swapchain();
  void collect_garbage(bool all);

  GLFWwindow* win_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t qfam_ = 0;
  std::string device_name_;
  VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;

  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D extent_{};
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkFramebuffer> framebuffers_;
  std::vector<VkSemaphore> rendered_;  // one per swapchain image
  VkImage msaa_image_ = VK_NULL_HANDLE;
  VkDeviceMemory msaa_mem_ = VK_NULL_HANDLE;
  VkImageView msaa_view_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;

  VkCommandPool pool_ = VK_NULL_HANDLE;
  Frame frames_[kFramesInFlight];
  int frame_ = 0;
  uint32_t image_ = 0;
  uint64_t frame_count_ = 0;
  bool rebuild_ = false;

  struct Garbage {
    Buffer b;
    uint64_t frame;
  };
  std::vector<Garbage> garbage_;
};

}  // namespace gsa::vk
