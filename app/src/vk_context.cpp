#include "vk_context.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace gsa::vk {

void check(VkResult r, const char* what) {
  if (r != VK_SUCCESS) throw Error(std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")");
}

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL on_debug(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
  if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr, "vulkan: %s\n", d->pMessage);
  return VK_FALSE;
}

bool has_layer(const char* name) {
  uint32_t n = 0;
  vkEnumerateInstanceLayerProperties(&n, nullptr);
  std::vector<VkLayerProperties> l(n);
  vkEnumerateInstanceLayerProperties(&n, l.data());
  return std::any_of(l.begin(), l.end(), [&](const auto& p) { return std::strcmp(p.layerName, name) == 0; });
}

}  // namespace

Context::Context(GLFWwindow* win, bool validation) : win_(win) {
  if (validation && !has_layer("VK_LAYER_KHRONOS_validation")) {
    std::fprintf(stderr, "vulkan: validation layer not installed, continuing without it\n");
    validation = false;
  }
  // ---- instance
  uint32_t n_ext = 0;
  const char** glfw_ext = glfwGetRequiredInstanceExtensions(&n_ext);
  if (!glfw_ext) throw Error("GLFW found no Vulkan support on this system");
  std::vector<const char*> ext(glfw_ext, glfw_ext + n_ext);
  if (validation) ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  const char* layers[] = {"VK_LAYER_KHRONOS_validation"};

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Grid of SA";
  app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app.pEngineName = "gsa";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = uint32_t(ext.size());
  ici.ppEnabledExtensionNames = ext.data();
  ici.enabledLayerCount = validation ? 1 : 0;
  ici.ppEnabledLayerNames = layers;
  check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance");

  if (validation) {
    VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    m.pfnUserCallback = on_debug;
    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (fn) fn(instance_, &m, nullptr, &messenger_);
  }

  check(glfwCreateWindowSurface(instance_, win_, nullptr, &surface_), "glfwCreateWindowSurface");

  // ---- physical device: discrete first, then integrated, then anything (software)
  uint32_t n_dev = 0;
  vkEnumeratePhysicalDevices(instance_, &n_dev, nullptr);
  if (n_dev == 0) throw Error("no Vulkan device found");
  std::vector<VkPhysicalDevice> devs(n_dev);
  vkEnumeratePhysicalDevices(instance_, &n_dev, devs.data());
  int best = -1;
  for (auto d : devs) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(d, &p);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qs.data());
    for (uint32_t i = 0; i < nq; ++i) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
      if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
      const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                 : 1;
      if (score > best) {
        best = score;
        phys_ = d;
        qfam_ = i;
        device_name_ = p.deviceName;
      }
      break;
    }
  }
  if (!phys_) throw Error("no Vulkan device can draw to this window");

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys_, &props);
  const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts;
  samples_ = (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;

  // ---- device
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = qfam_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dev_ext;
  check(vkCreateDevice(phys_, &dci, nullptr, &device_), "vkCreateDevice");
  vkGetDeviceQueue(device_, qfam_, 0, &queue_);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = qfam_;
  check(vkCreateCommandPool(device_, &pci, nullptr, &pool_), "vkCreateCommandPool");
  for (auto& f : frames_) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device_, &ai, &f.cmd), "vkAllocateCommandBuffers");
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(device_, &si, nullptr, &f.acquired), "vkCreateSemaphore");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device_, &fi, nullptr, &f.done), "vkCreateFence");
  }
  create_swapchain();
}

Context::~Context() {
  if (device_) vkDeviceWaitIdle(device_);
  collect_garbage(true);
  destroy_swapchain();
  if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
  for (auto& f : frames_) {
    vkDestroySemaphore(device_, f.acquired, nullptr);
    vkDestroyFence(device_, f.done, nullptr);
  }
  if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
  if (device_) vkDestroyDevice(device_, nullptr);
  if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (messenger_) {
    auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn) fn(instance_, messenger_, nullptr);
  }
  if (instance_) vkDestroyInstance(instance_, nullptr);
}

uint32_t Context::memory_type(uint32_t bits, VkMemoryPropertyFlags props) const {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  throw Error("no suitable memory type");
}

void Context::create_swapchain() {
  VkSurfaceCapabilitiesKHR caps;
  check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps), "surface caps");
  uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(nf);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, formats.data());
  // UNORM on purpose: colours are sRGB hex values from the design and blend like the web version
  VkSurfaceFormatKHR fmt = formats[0];
  for (const auto& f : formats)
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      fmt = f;
      break;
    }
  format_ = fmt.format;

  int fw = 0, fh = 0;
  glfwGetFramebufferSize(win_, &fw, &fh);
  if (caps.currentExtent.width != UINT32_MAX) extent_ = caps.currentExtent;
  else
    extent_ = {std::clamp(uint32_t(fw), caps.minImageExtent.width, caps.maxImageExtent.width),
               std::clamp(uint32_t(fh), caps.minImageExtent.height, caps.maxImageExtent.height)};

  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = count;
  sci.imageFormat = fmt.format;
  sci.imageColorSpace = fmt.colorSpace;
  sci.imageExtent = extent_;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);  // screenshots
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available; vsync
  sci.clipped = VK_TRUE;
  check(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_), "vkCreateSwapchainKHR");

  uint32_t ni = 0;
  vkGetSwapchainImagesKHR(device_, swapchain_, &ni, nullptr);
  images_.resize(ni);
  vkGetSwapchainImagesKHR(device_, swapchain_, &ni, images_.data());

  // render pass once (format and samples do not change)
  if (!render_pass_) {
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentDescription att[2]{};
    att[0].format = format_;
    att[0].samples = samples_;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att[1] = att[0];
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color;
    sub.pResolveAttachments = msaa ? &resolve : nullptr;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = msaa ? 2 : 1;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    check(vkCreateRenderPass(device_, &rpi, nullptr, &render_pass_), "vkCreateRenderPass");
  }

  if (samples_ != VK_SAMPLE_COUNT_1_BIT) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format_;
    ii.extent = {extent_.width, extent_.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = samples_;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    check(vkCreateImage(device_, &ii, nullptr, &msaa_image_), "vkCreateImage msaa");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, msaa_image_, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &ai, nullptr, &msaa_mem_), "vkAllocateMemory msaa");
    vkBindImageMemory(device_, msaa_image_, msaa_mem_, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = msaa_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device_, &vi, nullptr, &msaa_view_), "vkCreateImageView msaa");
  }

  views_.resize(ni);
  framebuffers_.resize(ni);
  rendered_.resize(ni);
  for (uint32_t i = 0; i < ni; ++i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = images_[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device_, &vi, nullptr, &views_[i]), "vkCreateImageView");
    VkImageView att[2] = {msaa_view_ ? msaa_view_ : views_[i], views_[i]};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = render_pass_;
    fi.attachmentCount = msaa_view_ ? 2 : 1;
    fi.pAttachments = att;
    fi.width = extent_.width;
    fi.height = extent_.height;
    fi.layers = 1;
    check(vkCreateFramebuffer(device_, &fi, nullptr, &framebuffers_[i]), "vkCreateFramebuffer");
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(device_, &si, nullptr, &rendered_[i]), "vkCreateSemaphore");
  }
}

void Context::destroy_swapchain() {
  for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
  for (auto v : views_) vkDestroyImageView(device_, v, nullptr);
  for (auto s : rendered_) vkDestroySemaphore(device_, s, nullptr);
  framebuffers_.clear();
  views_.clear();
  rendered_.clear();
  if (msaa_view_) vkDestroyImageView(device_, msaa_view_, nullptr);
  if (msaa_image_) vkDestroyImage(device_, msaa_image_, nullptr);
  if (msaa_mem_) vkFreeMemory(device_, msaa_mem_, nullptr);
  msaa_view_ = VK_NULL_HANDLE;
  msaa_image_ = VK_NULL_HANDLE;
  msaa_mem_ = VK_NULL_HANDLE;
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
}

bool Context::begin_frame(const float clear[4], const std::function<void(VkCommandBuffer)>& before_pass) {
  int fw = 0, fh = 0;
  glfwGetFramebufferSize(win_, &fw, &fh);
  if (fw == 0 || fh == 0) return false;  // minimised
  if (rebuild_ || uint32_t(fw) != extent_.width || uint32_t(fh) != extent_.height) {
    vkDeviceWaitIdle(device_);
    destroy_swapchain();
    create_swapchain();
    rebuild_ = false;
  }
  Frame& f = frames_[frame_];
  check(vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
  const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image_);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    rebuild_ = true;
    return false;
  }
  if (r != VK_SUBOPTIMAL_KHR) check(r, "vkAcquireNextImageKHR");
  vkResetFences(device_, 1, &f.done);
  collect_garbage(false);

  vkResetCommandBuffer(f.cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(f.cmd, &bi), "vkBeginCommandBuffer");
  if (before_pass) before_pass(f.cmd);
  VkClearValue cv{};
  std::memcpy(cv.color.float32, clear, sizeof(float) * 4);
  VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rbi.renderPass = render_pass_;
  rbi.framebuffer = framebuffers_[image_];
  rbi.renderArea = {{0, 0}, extent_};
  rbi.clearValueCount = 1;
  rbi.pClearValues = &cv;
  vkCmdBeginRenderPass(f.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vp{0, 0, float(extent_.width), float(extent_.height), 0, 1};
  VkRect2D sc{{0, 0}, extent_};
  vkCmdSetViewport(f.cmd, 0, 1, &vp);
  vkCmdSetScissor(f.cmd, 0, 1, &sc);
  return true;
}

void Context::end_frame(std::vector<uint8_t>* shot) {
  Frame& f = frames_[frame_];
  vkCmdEndRenderPass(f.cmd);
  Buffer readback;
  if (shot) {
    readback = create_buffer(VkDeviceSize(extent_.width) * extent_.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = images_[image_];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(f.cmd, images_[image_], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1, &c);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }
  check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");

  const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &f.acquired;
  si.pWaitDstStageMask = &wait;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &f.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &rendered_[image_];
  check(vkQueueSubmit(queue_, 1, &si, f.done), "vkQueueSubmit");

  if (shot) {
    vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);
    shot->resize(size_t(extent_.width) * extent_.height * 4);
    std::memcpy(shot->data(), readback.mapped, shot->size());
    if (format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB)
      for (size_t i = 0; i < shot->size(); i += 4) std::swap((*shot)[i], (*shot)[i + 2]);
    destroy_now(readback);
  }

  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &rendered_[image_];
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &image_;
  const VkResult r = vkQueuePresentKHR(queue_, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) rebuild_ = true;
  else check(r, "vkQueuePresentKHR");
  frame_ = (frame_ + 1) % kFramesInFlight;
  ++frame_count_;
}

Buffer Context::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
  Buffer b;
  b.size = size;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = std::max<VkDeviceSize>(size, 16);
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  check(vkCreateBuffer(device_, &bi, nullptr, &b.buf), "vkCreateBuffer");
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, b.buf, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex =
      memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  check(vkAllocateMemory(device_, &ai, nullptr, &b.mem), "vkAllocateMemory");
  vkBindBufferMemory(device_, b.buf, b.mem, 0);
  check(vkMapMemory(device_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
  return b;
}

void Context::destroy_now(Buffer& b) {
  if (b.mem) {
    vkUnmapMemory(device_, b.mem);
    vkFreeMemory(device_, b.mem, nullptr);
  }
  if (b.buf) vkDestroyBuffer(device_, b.buf, nullptr);
  b = Buffer{};
}

void Context::destroy_later(Buffer b) {
  if (b.buf) garbage_.push_back({b, frame_count_, {}});
}

void Context::destroy_later(std::function<void()> fn) {
  if (fn) garbage_.push_back({Buffer{}, frame_count_, std::move(fn)});
}

void Context::collect_garbage(bool all) {
  auto it = std::remove_if(garbage_.begin(), garbage_.end(), [&](Garbage& g) {
    if (all || frame_count_ >= g.frame + kFramesInFlight + 1) {
      destroy_now(g.b);
      if (g.fn) g.fn();
      return true;
    }
    return false;
  });
  garbage_.erase(it, garbage_.end());
}

VkShaderModule Context::shader(const uint32_t* spirv, size_t bytes) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes;
  ci.pCode = spirv;
  VkShaderModule m;
  check(vkCreateShaderModule(device_, &ci, nullptr, &m), "vkCreateShaderModule");
  return m;
}

}  // namespace gsa::vk
