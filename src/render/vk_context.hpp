#pragma once

#include <volk.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct SDL_Window;

namespace vkhb::render {

// One enumerated physical device and the queue families it offers for the app's surface. The two
// families are found independently: a GPU may render but not present (compute card, PRIME offload,
// a driver whose graphics family has no WSI support), and the pair may also just be different
// families on one device.
struct GpuInfo {
  VkPhysicalDevice handle = VK_NULL_HANDLE;
  std::string name;
  VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
  uint32_t graphics_family = UINT32_MAX;
  uint32_t present_family = UINT32_MAX;

  bool can_render() const { return graphics_family != UINT32_MAX; }
  bool can_present() const { return present_family != UINT32_MAX; }
};

const char* gpu_type_name(VkPhysicalDeviceType type);

// A logical device, its volk function table, and its queues. Move-only.
//
// Every device-level entry point goes through vk(); volk's device globals are deliberately left
// unloaded (volkLoadInstanceOnly, not volkLoadInstance) so a stray global device call fails loudly
// rather than dispatching through whichever device was loaded last — which is exactly the bug that
// would otherwise appear the moment a second device exists.
class Device {
 public:
  // pre: gpu.can_render(); enable_swapchain implies gpu.can_present().
  static std::expected<Device, std::string> create(const GpuInfo& gpu, bool enable_swapchain);

  Device() = default;
  Device(Device&& other) noexcept;
  Device& operator=(Device&& other) noexcept;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  ~Device();

  // pre: only valid on a device returned by create() (not a moved-from one).
  const VolkDeviceTable& vk() const { return *vk_; }

  VkDevice handle() const { return device_; }
  const GpuInfo& gpu() const { return gpu_; }

  VkQueue graphics_queue() const { return graphics_queue_; }
  uint32_t graphics_family() const { return gpu_.graphics_family; }

  // VK_NULL_HANDLE / UINT32_MAX unless this device was created with enable_swapchain.
  VkQueue present_queue() const { return present_queue_; }
  uint32_t present_family() const { return present_queue_ ? gpu_.present_family : UINT32_MAX; }

  // True when one queue both renders and presents — the fast path with no ownership transfer.
  bool unified_queue() const { return present_queue_ && gpu_.graphics_family == gpu_.present_family; }

  // First memory type satisfying both masks. The physical device's memory properties are queried
  // once in create() and cached, so this costs no Vulkan call.
  std::expected<uint32_t, std::string> find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags properties) const;

 private:
  void destroy() noexcept;

  std::unique_ptr<VolkDeviceTable> vk_;
  GpuInfo gpu_;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  VkQueue present_queue_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties mem_props_{};
};

// Owns the instance, debug messenger, surface, and one or two devices: the render device (always
// the GPU the caller asked for) and, when that GPU cannot present to the surface, a second
// present-capable device that owns the swapchain. Move-only.
//
// This is also where every instance-level Vulkan query lives. Physical-device and surface entry
// points have no volk table, so they are the library's only global calls; making them here once
// and caching the results (gpus(), surface_formats(), present_modes()) keeps them out of every
// other translation unit. surface_caps() is the one exception — see below.
class VulkanContext {
 public:
  // Always renders on the selected GPU; if it cannot present, pairs it with a present-capable one
  // rather than silently choosing a different renderer. A null `window` builds a surfaceless
  // context instead: no surface, no swapchain, present() == render() — for CI and screenshots.
  // `requested_present_gpu_index` forces the presenter (testing aid — normally auto-selected).
  static std::expected<VulkanContext, std::string> create(SDL_Window* window,
                                                          std::optional<uint32_t> requested_gpu_index,
                                                          std::optional<uint32_t> requested_present_gpu_index = {});

  // Every visible GPU and what it can do against `window`'s surface, without keeping anything
  // alive — for `--list-gpus`, which must report devices that no context could be built on.
  // pre: window valid.
  static std::expected<std::vector<GpuInfo>, std::string> probe_gpus(SDL_Window* window);

  VulkanContext(VulkanContext&& other) noexcept;
  VulkanContext& operator=(VulkanContext&& other) noexcept;
  VulkanContext(const VulkanContext&) = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;
  ~VulkanContext();

  // Where the application records all its rendering.
  const Device& render() const { return render_; }
  // Where the swapchain lives. Same object as render() unless cross_gpu().
  const Device& present() const { return present_ ? *present_ : render_; }
  // True when render and present are different physical devices, so frames must cross the PCIe gap
  // through host memory (see Presenter).
  bool cross_gpu() const { return present_.has_value(); }
  // True when created without a window: nothing can be presented, only rendered and read back.
  bool headless() const { return surface_ == VK_NULL_HANDLE; }

  VkSurfaceKHR surface() const { return surface_; }
  const std::vector<GpuInfo>& gpus() const { return gpus_; }

  // What the surface offers the present device. Fixed for the surface's lifetime, so both are
  // queried once in create(). Empty when headless().
  std::span<const VkSurfaceFormatKHR> surface_formats() const { return surface_formats_; }
  std::span<const VkPresentModeKHR> present_modes() const { return present_modes_; }

  // Deliberately *not* cached: currentExtent follows the window, and a stale copy is how a
  // swapchain ends up rebuilt at yesterday's size. pre: !headless().
  VkSurfaceCapabilitiesKHR surface_caps() const;

 private:
  VulkanContext() = default;
  // Fills surface_formats_/present_modes_ for the present device. pre: surface_ and the devices set.
  void cache_surface_info();
  void destroy() noexcept;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  Device render_;
  std::optional<Device> present_;
  std::vector<GpuInfo> gpus_;
  std::vector<VkSurfaceFormatKHR> surface_formats_;
  std::vector<VkPresentModeKHR> present_modes_;
};

}  // namespace vkhb::render
