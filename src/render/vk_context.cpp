#include "vk_context.hpp"

#include "vk_error.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cassert>
#include <print>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace vkhb::render {

namespace {

constexpr bool kWantValidation =
#ifndef NDEBUG
    true;
#else
    false;
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* /*user_data*/) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::println(stderr, "[vulkan] {}", data->pMessage);
  return VK_FALSE;
}

bool has_extension(std::span<const VkExtensionProperties> available, std::string_view name) {
  return std::ranges::any_of(available, [&](const auto& ext) { return name == ext.extensionName; });
}

bool has_layer(std::span<const VkLayerProperties> available, std::string_view name) {
  return std::ranges::any_of(available, [&](const auto& layer) { return name == layer.layerName; });
}

// Enumerates a two-call instance-level query into a vector. Both calls are global: volk has no
// table for instance entry points.
template <typename T, typename Fn>
std::vector<T> enumerated(Fn&& query) {
  uint32_t count = 0;
  query(&count, nullptr);
  std::vector<T> out(count);
  query(&count, out.data());
  return out;
}

// Graphics and present are looked up separately: a family with both is preferred (it avoids every
// ownership transfer), but a device offering them only in different families is still fully usable,
// and one offering no present family at all can still render.
void find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface, GpuInfo& out) {
  const auto families = enumerated<VkQueueFamilyProperties>([&](uint32_t* n, VkQueueFamilyProperties* p) {
    vkGetPhysicalDeviceQueueFamilyProperties(device, n, p);
  });

  const auto presents = [&](uint32_t i) {
    if (surface == VK_NULL_HANDLE) return false;  // headless: nothing to present to
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supported);
    return supported == VK_TRUE;
  };

  for (uint32_t i = 0; i < families.size(); ++i) {
    const bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    if (graphics && presents(i)) {  // one family doing both always wins
      out.graphics_family = i;
      out.present_family = i;
      return;
    }
    if (graphics && out.graphics_family == UINT32_MAX) out.graphics_family = i;
    if (presents(i) && out.present_family == UINT32_MAX) out.present_family = i;
  }
}

// Every physical device visible to `instance`, with each one's graphics and present family (either
// may be absent). `surface` may be VK_NULL_HANDLE, in which case no device reports a present family.
std::vector<GpuInfo> enumerate_gpus(VkInstance instance, VkSurfaceKHR surface) {
  const auto handles = enumerated<VkPhysicalDevice>(
      [&](uint32_t* n, VkPhysicalDevice* p) { vkEnumeratePhysicalDevices(instance, n, p); });

  std::vector<GpuInfo> gpus;
  gpus.reserve(handles.size());
  for (VkPhysicalDevice handle : handles) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(handle, &props);
    GpuInfo info{.handle = handle, .name = props.deviceName, .type = props.deviceType};
    find_queue_families(handle, surface, info);
    gpus.push_back(std::move(info));
  }
  return gpus;
}

// An index into `gpus`: `requested` if in range (even if unusable, so the caller can report a
// precise error), else the first renderable discrete GPU, then any renderable GPU, then 0.
// pre: !gpus.empty().
size_t choose_gpu(const std::vector<GpuInfo>& gpus, std::optional<uint32_t> requested) {
  assert(!gpus.empty());
  if (requested && *requested < gpus.size()) return *requested;

  const auto renders = [](const GpuInfo& g) { return g.can_render(); };
  if (auto it = std::ranges::find_if(
          gpus, [&](const GpuInfo& g) { return renders(g) && g.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU; });
      it != gpus.end())
    return static_cast<size_t>(it - gpus.begin());
  if (auto it = std::ranges::find_if(gpus, renders); it != gpus.end())
    return static_cast<size_t>(it - gpus.begin());
  return 0;
}

struct Instance {
  VkInstance handle = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
};

// The instance both paths share: the windowed one adds SDL's surface extensions, the surfaceless
// one needs none. Validation and the debug messenger are enabled in debug builds when the loader
// has them. post: on success volk holds instance-level entry points only.
std::expected<Instance, std::string> create_instance(bool want_surface) {
  if (volkInitialize() != VK_SUCCESS) return std::unexpected("volkInitialize failed (no Vulkan loader found)");

  const auto avail_exts = enumerated<VkExtensionProperties>(
      [](uint32_t* n, VkExtensionProperties* p) { vkEnumerateInstanceExtensionProperties(nullptr, n, p); });
  const auto avail_layers =
      enumerated<VkLayerProperties>([](uint32_t* n, VkLayerProperties* p) { vkEnumerateInstanceLayerProperties(n, p); });

  std::vector<const char*> exts;
  if (want_surface) {
    uint32_t sdl_count = 0;
    char const* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
    if (!sdl_exts) return std::unexpected(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    exts.assign(sdl_exts, sdl_exts + sdl_count);
  }
  const bool debug_utils = kWantValidation && has_extension(avail_exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  if (debug_utils) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  std::vector<const char*> layers;
  if (kWantValidation && has_layer(avail_layers, "VK_LAYER_KHRONOS_validation"))
    layers.push_back("VK_LAYER_KHRONOS_validation");

  const VkApplicationInfo app_info{
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vkhb_app",
      .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
      .pEngineName = "vkhb_app",
      .engineVersion = VK_MAKE_VERSION(0, 1, 0),
      .apiVersion = VK_API_VERSION_1_3,
  };
  const VkInstanceCreateInfo instance_info{
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
      .enabledLayerCount = static_cast<uint32_t>(layers.size()),
      .ppEnabledLayerNames = layers.data(),
      .enabledExtensionCount = static_cast<uint32_t>(exts.size()),
      .ppEnabledExtensionNames = exts.data(),
  };
  Instance inst;
  if (const VkResult r = vkCreateInstance(&instance_info, nullptr, &inst.handle); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateInstance", r));

  // Instance-level entry points only — device functions live in each Device's volk table.
  volkLoadInstanceOnly(inst.handle);

  if (debug_utils) {
    const VkDebugUtilsMessengerCreateInfoEXT dbg_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback,
    };
    vkCreateDebugUtilsMessengerEXT(inst.handle, &dbg_info, nullptr, &inst.messenger);
  }
  return inst;
}

}  // namespace

const char* gpu_type_name(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
  }
}

// --- Device ---

std::expected<Device, std::string> Device::create(const GpuInfo& gpu, bool enable_swapchain) {
  assert(gpu.can_render());
  assert(!enable_swapchain || gpu.can_present());

  Device dev;
  dev.gpu_ = gpu;
  vkGetPhysicalDeviceMemoryProperties(gpu.handle, &dev.mem_props_);

  VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceFeatures2 features2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features13};
  vkGetPhysicalDeviceFeatures2(gpu.handle, &features2);
  if (!features13.dynamicRendering || !features13.synchronization2)
    return std::unexpected("device '" + gpu.name + "' lacks Vulkan 1.3 dynamicRendering/synchronization2");
  features13.dynamicRendering = VK_TRUE;
  features13.synchronization2 = VK_TRUE;

  // One VkDeviceQueueCreateInfo per distinct family: the spec forbids naming a family twice.
  const float priority = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> queue_infos;
  const auto want_family = [&](uint32_t family) {
    if (family == UINT32_MAX) return;
    if (std::ranges::any_of(queue_infos, [&](const auto& q) { return q.queueFamilyIndex == family; })) return;
    queue_infos.push_back({.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                           .queueFamilyIndex = family,
                           .queueCount = 1,
                           .pQueuePriorities = &priority});
  };
  want_family(gpu.graphics_family);
  if (enable_swapchain) want_family(gpu.present_family);

  std::vector<const char*> extensions;
  if (enable_swapchain) {
    const auto available = enumerated<VkExtensionProperties>([&](uint32_t* n, VkExtensionProperties* p) {
      vkEnumerateDeviceExtensionProperties(gpu.handle, nullptr, n, p);
    });
    if (!has_extension(available, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
      return std::unexpected("device '" + gpu.name + "' cannot present: no VK_KHR_swapchain");
    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }

  const VkDeviceCreateInfo device_info{
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features2,
      .queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size()),
      .pQueueCreateInfos = queue_infos.data(),
      .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
      .ppEnabledExtensionNames = extensions.data(),
  };
  if (const VkResult r = vkCreateDevice(gpu.handle, &device_info, nullptr, &dev.device_); r != VK_SUCCESS)
    return std::unexpected(vk_error(("vkCreateDevice on '" + gpu.name + "'").c_str(), r));

  dev.vk_ = std::make_unique<VolkDeviceTable>();
  volkLoadDeviceTable(dev.vk_.get(), dev.device_);
  dev.vk_->vkGetDeviceQueue(dev.device_, gpu.graphics_family, 0, &dev.graphics_queue_);
  if (enable_swapchain) dev.vk_->vkGetDeviceQueue(dev.device_, gpu.present_family, 0, &dev.present_queue_);
  return dev;
}

std::expected<uint32_t, std::string> Device::find_memory_type(uint32_t type_bits,
                                                              VkMemoryPropertyFlags properties) const {
  for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
    const bool type_ok = type_bits & (1u << i);
    const bool props_ok = (mem_props_.memoryTypes[i].propertyFlags & properties) == properties;
    if (type_ok && props_ok) return i;
  }
  return std::unexpected("no suitable Vulkan memory type for requested properties");
}

void Device::destroy() noexcept {
  if (device_) vk_->vkDestroyDevice(device_, nullptr);
  device_ = VK_NULL_HANDLE;
  graphics_queue_ = VK_NULL_HANDLE;
  present_queue_ = VK_NULL_HANDLE;
}

Device::Device(Device&& other) noexcept
    : vk_(std::move(other.vk_)),
      gpu_(std::exchange(other.gpu_, GpuInfo{})),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      graphics_queue_(std::exchange(other.graphics_queue_, VK_NULL_HANDLE)),
      present_queue_(std::exchange(other.present_queue_, VK_NULL_HANDLE)),
      mem_props_(other.mem_props_) {}

Device& Device::operator=(Device&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  vk_ = std::move(other.vk_);
  gpu_ = std::exchange(other.gpu_, GpuInfo{});
  device_ = std::exchange(other.device_, VK_NULL_HANDLE);
  graphics_queue_ = std::exchange(other.graphics_queue_, VK_NULL_HANDLE);
  present_queue_ = std::exchange(other.present_queue_, VK_NULL_HANDLE);
  mem_props_ = other.mem_props_;
  return *this;
}

Device::~Device() { destroy(); }

// --- VulkanContext ---

std::expected<VulkanContext, std::string> VulkanContext::create(
    SDL_Window* window, std::optional<uint32_t> requested_gpu_index,
    std::optional<uint32_t> requested_present_gpu_index) {
  auto inst = create_instance(/*want_surface=*/window != nullptr);
  if (!inst) return std::unexpected(inst.error());

  VulkanContext ctx;
  ctx.instance_ = inst->handle;
  ctx.debug_messenger_ = inst->messenger;

  if (window && !SDL_Vulkan_CreateSurface(window, ctx.instance_, nullptr, &ctx.surface_))
    return std::unexpected(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());

  ctx.gpus_ = enumerate_gpus(ctx.instance_, ctx.surface_);
  if (ctx.gpus_.empty()) return std::unexpected("no Vulkan physical devices found");
  const GpuInfo& render_gpu = ctx.gpus_[choose_gpu(ctx.gpus_, requested_gpu_index)];
  if (!render_gpu.can_render())
    return std::unexpected("device '" + render_gpu.name + "' has no graphics queue family");

  // Surfaceless: one render-only device and nothing to query the surface about.
  if (!window) {
    auto dev = Device::create(render_gpu, /*enable_swapchain=*/false);
    if (!dev) return std::unexpected(dev.error());
    ctx.render_ = std::move(*dev);
    return ctx;
  }

  // The requested GPU always stays the renderer. If it can also present, it owns the swapchain too;
  // otherwise it is paired with a present-capable GPU and frames cross through host memory.
  const bool present_is_render =
      requested_present_gpu_index && *requested_present_gpu_index < ctx.gpus_.size() &&
      ctx.gpus_[*requested_present_gpu_index].handle == render_gpu.handle;
  if (render_gpu.can_present() && (!requested_present_gpu_index || present_is_render)) {
    auto dev = Device::create(render_gpu, /*enable_swapchain=*/true);
    if (!dev) return std::unexpected(dev.error());
    ctx.render_ = std::move(*dev);
    ctx.cache_surface_info();
    return ctx;
  }

  const auto present_index = [&]() -> std::optional<size_t> {
    if (requested_present_gpu_index && *requested_present_gpu_index < ctx.gpus_.size()) {
      return ctx.gpus_[*requested_present_gpu_index].can_present()
                 ? std::optional<size_t>(*requested_present_gpu_index)
                 : std::nullopt;
    }
    auto it = std::ranges::find_if(ctx.gpus_, [](const GpuInfo& g) { return g.can_present(); });
    return it == ctx.gpus_.end() ? std::nullopt : std::optional<size_t>(it - ctx.gpus_.begin());
  }();
  if (!present_index)
    return std::unexpected("device '" + render_gpu.name +
                           "' cannot present and no other device can present to this surface");

  auto render_dev = Device::create(render_gpu, /*enable_swapchain=*/false);
  if (!render_dev) return std::unexpected(render_dev.error());
  auto present_dev = Device::create(ctx.gpus_[*present_index], /*enable_swapchain=*/true);
  if (!present_dev) return std::unexpected(present_dev.error());
  ctx.render_ = std::move(*render_dev);
  ctx.present_ = std::move(*present_dev);
  ctx.cache_surface_info();
  return ctx;
}

std::expected<std::vector<GpuInfo>, std::string> VulkanContext::probe_gpus(SDL_Window* window) {
  // want_surface only with a window: SDL_Vulkan_GetInstanceExtensions dereferences the video driver,
  // which is null when SDL_INIT_VIDEO failed.
  auto inst = create_instance(/*want_surface=*/window != nullptr);
  if (!inst) return std::unexpected(inst.error());

  // A null window, or a surface that fails to build, is not fatal here: every device then simply
  // reports present=no.
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (window) SDL_Vulkan_CreateSurface(window, inst->handle, nullptr, &surface);
  std::vector<GpuInfo> gpus = enumerate_gpus(inst->handle, surface);

  if (surface) vkDestroySurfaceKHR(inst->handle, surface, nullptr);
  if (inst->messenger) vkDestroyDebugUtilsMessengerEXT(inst->handle, inst->messenger, nullptr);
  vkDestroyInstance(inst->handle, nullptr);
  return gpus;
}

void VulkanContext::cache_surface_info() {
  const VkPhysicalDevice physical = present().gpu().handle;
  surface_formats_ = enumerated<VkSurfaceFormatKHR>([&](uint32_t* n, VkSurfaceFormatKHR* p) {
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, n, p);
  });
  present_modes_ = enumerated<VkPresentModeKHR>(
      [&](uint32_t* n, VkPresentModeKHR* p) { vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, n, p); });
}

VkSurfaceCapabilitiesKHR VulkanContext::surface_caps() const {
  assert(!headless());
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(present().gpu().handle, surface_, &caps);
  return caps;
}

VulkanContext::VulkanContext(VulkanContext&& other) noexcept
    : instance_(std::exchange(other.instance_, VK_NULL_HANDLE)),
      debug_messenger_(std::exchange(other.debug_messenger_, VK_NULL_HANDLE)),
      surface_(std::exchange(other.surface_, VK_NULL_HANDLE)),
      render_(std::move(other.render_)),
      present_(std::move(other.present_)),
      gpus_(std::move(other.gpus_)),
      surface_formats_(std::move(other.surface_formats_)),
      present_modes_(std::move(other.present_modes_)) {}

VulkanContext& VulkanContext::operator=(VulkanContext&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
  debug_messenger_ = std::exchange(other.debug_messenger_, VK_NULL_HANDLE);
  surface_ = std::exchange(other.surface_, VK_NULL_HANDLE);
  render_ = std::move(other.render_);
  present_ = std::move(other.present_);
  gpus_ = std::move(other.gpus_);
  surface_formats_ = std::move(other.surface_formats_);
  present_modes_ = std::move(other.present_modes_);
  return *this;
}

void VulkanContext::destroy() noexcept {
  present_.reset();  // devices must die before the surface/instance they were made against
  render_ = Device{};
  if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (debug_messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
  if (instance_) vkDestroyInstance(instance_, nullptr);
  surface_ = VK_NULL_HANDLE;
  debug_messenger_ = VK_NULL_HANDLE;
  instance_ = VK_NULL_HANDLE;
}

VulkanContext::~VulkanContext() { destroy(); }

}  // namespace vkhb::render
