#pragma once
// Vertex memory for map tiles: a few large blocks carved into slices, instead of one Vulkan
// allocation per tile. vkAllocateMemory is slow (milliseconds on some drivers) and drivers cap the
// number of allocations, so a tile's mesh lands in a slice of a 32 MB block. Blocks prefer memory
// that is both on the GPU and writable from the CPU (the BAR window that every discrete GPU has),
// so drawing does not pull vertices across the bus every frame; otherwise plain host memory.

#include <cstdint>
#include <mutex>
#include <vector>

#include <vulkan/vulkan.h>

namespace gsa::vk {
class Context;

struct Slice {
  VkBuffer buf = VK_NULL_HANDLE;  // the block's buffer
  VkDeviceSize offset = 0, size = 0;
  void* mapped = nullptr;         // CPU address of the slice
  int block = -1;
  explicit operator bool() const { return buf != VK_NULL_HANDLE; }
};

class VertexPool {
 public:
  explicit VertexPool(Context& ctx);
  ~VertexPool();
  VertexPool(const VertexPool&) = delete;
  VertexPool& operator=(const VertexPool&) = delete;
  Slice alloc(VkDeviceSize size);  // any thread
  void free(const Slice& s);  // any thread; only once the GPU no longer reads it (see Context::destroy_later)
  VkDeviceSize used() const { return used_; }
  bool device_local() const { return device_local_; }

 private:
  struct Range {
    VkDeviceSize offset, size;
  };
  struct Block {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    char* mapped = nullptr;
    std::vector<Range> free;  // sorted by offset, coalesced
  };
  bool add_block(VkDeviceSize size);
  Context& ctx_;
  std::mutex mu_;
  std::vector<Block> blocks_;
  VkDeviceSize used_ = 0;
  bool device_local_ = false;
  int fast_type_ = -1, host_type_ = -1;
};

}  // namespace gsa::vk
