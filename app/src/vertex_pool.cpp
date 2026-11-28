#include "vertex_pool.hpp"

#include <algorithm>

#include "vk_context.hpp"

namespace gsa::vk {
namespace {
constexpr VkDeviceSize kBlock = 32ull << 20;
constexpr VkDeviceSize kAlign = 256;
VkDeviceSize align_up(VkDeviceSize v) { return (v + kAlign - 1) & ~(kAlign - 1); }
}  // namespace

VertexPool::VertexPool(Context& ctx) : ctx_(ctx) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(ctx_.physical(), &mp);
  const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    const auto f = mp.memoryTypes[i].propertyFlags;
    if ((f & host) != host) continue;
    if (host_type_ < 0) host_type_ = int(i);
    // on the GPU and mapped: worth it when the heap is not tiny
    if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && fast_type_ < 0 &&
        mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size >= (128ull << 20))
      fast_type_ = int(i);
  }
}

VertexPool::~VertexPool() {
  const VkDevice dev = ctx_.device();
  for (auto& b : blocks_) {
    vkUnmapMemory(dev, b.mem);
    vkDestroyBuffer(dev, b.buf, nullptr);
    vkFreeMemory(dev, b.mem, nullptr);
  }
}

bool VertexPool::add_block(VkDeviceSize size) {
  const VkDevice dev = ctx_.device();
  Block b;
  b.size = size;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(dev, &bi, nullptr, &b.buf) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev, b.buf, &req);
  for (int type : {fast_type_, host_type_}) {
    if (type < 0 || !(req.memoryTypeBits & (1u << type))) continue;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(type);
    if (vkAllocateMemory(dev, &ai, nullptr, &b.mem) != VK_SUCCESS) {
      if (type == fast_type_) fast_type_ = -1;  // the GPU window is full: host memory from now on
      continue;
    }
    vkBindBufferMemory(dev, b.buf, b.mem, 0);
    void* p = nullptr;
    vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p);
    b.mapped = static_cast<char*>(p);
    b.free.push_back({0, size});
    if (type == fast_type_) device_local_ = true;
    blocks_.push_back(std::move(b));
    return true;
  }
  vkDestroyBuffer(dev, b.buf, nullptr);
  return false;
}

Slice VertexPool::alloc(VkDeviceSize size) {
  std::lock_guard<std::mutex> lk(mu_);
  size = align_up(std::max<VkDeviceSize>(size, 16));
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t bi = 0; bi < blocks_.size(); ++bi) {
      auto& b = blocks_[bi];
      for (size_t i = 0; i < b.free.size(); ++i) {
        auto& r = b.free[i];
        if (r.size < size) continue;
        Slice s{b.buf, r.offset, size, b.mapped + r.offset, int(bi)};
        r.offset += size;
        r.size -= size;
        if (r.size == 0) b.free.erase(b.free.begin() + long(i));
        used_ += size;
        return s;
      }
    }
    if (pass == 0 && !add_block(std::max(kBlock, size))) break;
  }
  return {};
}

void VertexPool::free(const Slice& s) {
  if (!s) return;
  std::lock_guard<std::mutex> lk(mu_);
  if (s.block < 0 || size_t(s.block) >= blocks_.size()) return;
  auto& f = blocks_[size_t(s.block)].free;
  auto it = std::lower_bound(f.begin(), f.end(), s.offset, [](const Range& r, VkDeviceSize o) { return r.offset < o; });
  it = f.insert(it, {s.offset, s.size});
  // merge with the next and the previous free range
  if (auto nx = it + 1; nx != f.end() && it->offset + it->size == nx->offset) {
    it->size += nx->size;
    f.erase(nx);
  }
  if (it != f.begin()) {
    auto pv = it - 1;
    if (pv->offset + pv->size == it->offset) {
      pv->size += it->size;
      f.erase(it);
    }
  }
  used_ -= s.size;
}

}  // namespace gsa::vk
