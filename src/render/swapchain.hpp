#pragma once

#include <volk.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace vkhb::render {

class VulkanContext;

// What to ask the surface for. Each mode falls back down the list when the surface does not offer
// it, so any value is always satisfiable; Swapchain::present_mode() reports what was actually
// chosen. FIFO is the only mode the spec guarantees exists.
//
//   Vsync     FIFO. Presents are queued and shown one per refresh: no tearing, frame rate capped
//             to the display, and the queue is what paces the whole loop.
//   Mailbox   MAILBOX, else FIFO. No tearing either, but the app renders as fast as it likes and
//             the display picks up the newest finished frame, dropping the rest. Wants a third
//             swapchain image to have somewhere to keep that spare frame.
//   Immediate IMMEDIATE, else MAILBOX, else FIFO. Scans out mid-frame — tears visibly — in
//             exchange for the lowest latency available.
enum class PresentPreference : uint8_t { Vsync, Mailbox, Immediate };

// Human-readable name for a preference ("vsync", "mailbox", "immediate"); for CLI parsing and logs.
const char* present_preference_name(PresentPreference pref);

// The swapchain, always owned by the context's *present* device (which may not be the device the
// app renders on — see Presenter). Sized in physical pixels: pass SDL_GetWindowSizeInPixels, not
// SDL_GetWindowSize. recreate() retires the old swapchain via oldSwapchain so in-flight presents
// finish cleanly.
//
// Images are acquired as transfer destinations, not colour attachments: frames are produced on the
// render device and copied in. No image views are created — nothing renders into these directly.
class Swapchain {
 public:
  static std::expected<Swapchain, std::string> create(const VulkanContext& ctx, uint32_t width, uint32_t height,
                                                      PresentPreference pref = PresentPreference::Vsync);

  Swapchain() = default;
  Swapchain(Swapchain&& other) noexcept;
  Swapchain& operator=(Swapchain&& other) noexcept;
  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;
  ~Swapchain();

  // pre: width>0, height>0 (skip recreation while the window is minimized).
  std::expected<void, std::string> recreate(const VulkanContext& ctx, uint32_t width, uint32_t height,
                                            PresentPreference pref = PresentPreference::Vsync);

  VkSwapchainKHR handle() const { return swapchain_; }
  VkFormat format() const { return format_; }
  VkExtent2D extent() const { return extent_; }
  VkPresentModeKHR present_mode() const { return present_mode_; }  // what was actually chosen
  const std::vector<VkImage>& images() const { return images_; }

 private:
  static std::expected<void, std::string> build(const VulkanContext& ctx, uint32_t width, uint32_t height,
                                                PresentPreference pref, VkSwapchainKHR old, Swapchain& out);
  void destroy() noexcept;

  const VolkDeviceTable* vk_ = nullptr;  // non-owning, borrowed from the present Device
  VkDevice device_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
  VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
  std::vector<VkImage> images_;
};

}  // namespace vkhb::render
