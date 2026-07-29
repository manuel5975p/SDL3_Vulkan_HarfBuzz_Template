#include "swapchain.hpp"

#include "vk_context.hpp"
#include "vk_error.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <span>
#include <utility>

namespace vkhb::render {

namespace {

VkSurfaceFormatKHR choose_surface_format(std::span<const VkSurfaceFormatKHR> formats) {
  for (const auto& f : formats)
    if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
  return formats.front();
}

// Falls through to progressively more conservative modes, so every preference resolves to something
// the surface actually offers. FIFO terminates the chain — the spec requires every surface to
// support it.
VkPresentModeKHR choose_present_mode(std::span<const VkPresentModeKHR> modes, PresentPreference pref) {
  const auto offered = [modes](VkPresentModeKHR m) { return std::ranges::contains(modes, m); };
  switch (pref) {
    case PresentPreference::Immediate:
      if (offered(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
      [[fallthrough]];  // no tear-free low-latency mode is worse than mailbox, so try that next
    case PresentPreference::Mailbox:
      if (offered(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
      [[fallthrough]];
    case PresentPreference::Vsync: break;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

// How many images the swapchain needs. minImageCount is the count at which the app is *allowed* to
// run, not the count at which it runs well: with exactly that many, every acquire blocks until the
// presentation engine hands one back. One spare decouples the two.
//
// MAILBOX needs three regardless of what the driver reports as its minimum. The whole point of the
// mode is that a finished frame can sit in the mailbox waiting for the next refresh while the app
// renders its replacement — that is one image being displayed, one waiting, one being drawn. With
// two, the app blocks in acquire exactly as it would under FIFO and the mode buys nothing.
uint32_t choose_image_count(const VkSurfaceCapabilitiesKHR& caps, VkPresentModeKHR mode) {
  uint32_t count = caps.minImageCount + 1;
  if (mode == VK_PRESENT_MODE_MAILBOX_KHR) count = std::max(count, 3u);
  if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);
  return count;
}

VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& caps, uint32_t width, uint32_t height) {
  if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;
  return VkExtent2D{
      .width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
      .height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height),
  };
}

}  // namespace

std::expected<void, std::string> Swapchain::build(const VulkanContext& ctx, uint32_t width, uint32_t height,
                                                  PresentPreference pref, VkSwapchainKHR old, Swapchain& out) {
  const Device& dev = ctx.present();
  const VolkDeviceTable& vk = dev.vk();

  // Formats and modes were queried once when the context was built; only the capabilities have to
  // be fresh, because currentExtent follows the window.
  const auto formats = ctx.surface_formats();
  if (formats.empty()) return std::unexpected("surface exposes no formats");
  const VkSurfaceCapabilitiesKHR caps = ctx.surface_caps();

  const VkSurfaceFormatKHR surface_format = choose_surface_format(formats);
  const VkExtent2D extent = choose_extent(caps, width, height);

  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    return std::unexpected("surface does not support VK_IMAGE_USAGE_TRANSFER_DST_BIT swapchain images");

  // Mode first: it decides how many images are worth asking for.
  const VkPresentModeKHR present_mode = choose_present_mode(ctx.present_modes(), pref);
  const uint32_t image_count = choose_image_count(caps, present_mode);

  // The image is filled on the graphics family but presented on the present family. When those
  // differ, CONCURRENT sharing lets both touch it without an explicit ownership transfer — a
  // second command buffer on the present queue just to hand the image over would cost more than
  // the concurrent-access penalty on the one full-screen copy per frame.
  const std::array<uint32_t, 2> families{dev.graphics_family(), dev.present_family()};
  const bool split_families = families[0] != families[1];
  const VkSwapchainCreateInfoKHR info{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = ctx.surface(),
      .minImageCount = image_count,
      .imageFormat = surface_format.format,
      .imageColorSpace = surface_format.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = split_families ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = split_families ? 2u : 0u,
      .pQueueFamilyIndices = split_families ? families.data() : nullptr,
      .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = present_mode,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
  };

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  if (const VkResult r = vk.vkCreateSwapchainKHR(dev.handle(), &info, nullptr, &swapchain); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateSwapchainKHR", r));

  uint32_t actual_image_count = 0;
  vk.vkGetSwapchainImagesKHR(dev.handle(), swapchain, &actual_image_count, nullptr);
  std::vector<VkImage> images(actual_image_count);
  vk.vkGetSwapchainImagesKHR(dev.handle(), swapchain, &actual_image_count, images.data());

  out.vk_ = &vk;
  out.device_ = dev.handle();
  out.swapchain_ = swapchain;
  out.format_ = surface_format.format;
  out.extent_ = extent;
  out.present_mode_ = info.presentMode;
  out.images_ = std::move(images);
  return {};
}

const char* present_preference_name(PresentPreference pref) {
  switch (pref) {
    case PresentPreference::Vsync: return "vsync";
    case PresentPreference::Mailbox: return "mailbox";
    case PresentPreference::Immediate: return "immediate";
  }
  return "vsync";
}

std::expected<Swapchain, std::string> Swapchain::create(const VulkanContext& ctx, uint32_t width, uint32_t height,
                                                        PresentPreference pref) {
  Swapchain sc;
  if (auto r = build(ctx, width, height, pref, VK_NULL_HANDLE, sc); !r) return std::unexpected(r.error());
  return sc;
}

std::expected<void, std::string> Swapchain::recreate(const VulkanContext& ctx, uint32_t width, uint32_t height,
                                                     PresentPreference pref) {
  const VkSwapchainKHR old_swapchain = swapchain_;

  Swapchain fresh;
  if (auto r = build(ctx, width, height, pref, old_swapchain, fresh); !r) return std::unexpected(r.error());

  if (old_swapchain) vk_->vkDestroySwapchainKHR(device_, old_swapchain, nullptr);
  swapchain_ = VK_NULL_HANDLE;  // destroyed above; operator= below must not destroy it again

  *this = std::move(fresh);
  return {};
}

void Swapchain::destroy() noexcept {
  if (vk_ && swapchain_) vk_->vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  images_.clear();
  swapchain_ = VK_NULL_HANDLE;
}

Swapchain::Swapchain(Swapchain&& other) noexcept
    : vk_(std::exchange(other.vk_, nullptr)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      swapchain_(std::exchange(other.swapchain_, VK_NULL_HANDLE)),
      format_(std::exchange(other.format_, VK_FORMAT_UNDEFINED)),
      extent_(std::exchange(other.extent_, VkExtent2D{})),
      present_mode_(other.present_mode_),
      images_(std::move(other.images_)) {}

Swapchain& Swapchain::operator=(Swapchain&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  vk_ = std::exchange(other.vk_, nullptr);
  device_ = std::exchange(other.device_, VK_NULL_HANDLE);
  swapchain_ = std::exchange(other.swapchain_, VK_NULL_HANDLE);
  format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
  extent_ = std::exchange(other.extent_, VkExtent2D{});
  present_mode_ = other.present_mode_;
  images_ = std::move(other.images_);
  return *this;
}

Swapchain::~Swapchain() { destroy(); }

}  // namespace vkhb::render
