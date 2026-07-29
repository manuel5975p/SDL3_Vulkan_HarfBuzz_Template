#pragma once

// Frame orchestration: hands the app an offscreen colour target on the *render* device, then gets
// that image onto the screen whichever way the hardware allows.
//
// The app never renders into a swapchain image directly. It always renders into an offscreen
// B8G8R8A8_UNORM target owned by the render device, which buys two things:
//   * The render GPU need not be able to present at all. When it cannot, frames reach a second,
//     present-capable GPU through host memory (the only portable cross-device path — external
//     memory handles are driver- and platform-specific).
//   * Alpha blends on raw gamma-encoded bytes, CSS-style, on every device — no dependence on
//     VK_KHR_swapchain_mutable_format to get an sRGB-space blend.
// The cost on the same-GPU path is one full-screen image copy per frame.
//
// The cross-GPU path presents one frame behind. No semaphore spans two devices, so the handover is
// a CPU wait on the render device followed by a host copy; doing that inline would serialise the
// whole pipeline (render, copy and present would sum instead of overlap) and make the frame long
// enough that the present mode never became the limiter. Instead end_frame() submits frame N and
// then hands over frame N-1, which the render GPU finished a whole frame ago — so the acquire, and
// with it the present mode, is what paces the loop again.
//
// Both paths acquire *late* — in end_frame(), after the app has finished recording. vkAcquireNext-
// ImageKHR blocks the calling thread until the presentation engine gives an image back, which under
// FIFO means "until the next refresh"; acquiring in begin_frame() would put that wait in front of
// the frame's CPU work, so recording and the vsync wait would add up instead of overlapping. The
// only thing the acquire has to precede is the copy into the swapchain image, which is the last
// thing recorded.

#include "swapchain.hpp"
#include "vk_buffer.hpp"
#include "vk_image.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

#include <volk.h>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace vkhb::render {

class VulkanContext;

inline constexpr uint32_t kFramesInFlight = 2;

// What the app draws into for one frame. `cmd` is already recording and the target is already
// cleared and in COLOR_ATTACHMENT_OPTIMAL.
struct FrameTarget {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkImageView color_view = VK_NULL_HANDLE;
  VkImage color_image = VK_NULL_HANDLE;  // same image as color_view, for post-process barriers
  VkExtent2D extent{};
  uint32_t frame_index = 0;  // 0..kFramesInFlight-1, for the caller's own per-frame resources
};

class Presenter {
 public:
  // Every offscreen target and pipeline that draws into one uses this format.
  static constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM;

  // pre: width/height > 0, in physical pixels.
  static std::expected<Presenter, std::string> create(const VulkanContext& ctx, uint32_t width, uint32_t height);

  Presenter() = default;
  Presenter(Presenter&& other) noexcept;
  Presenter& operator=(Presenter&& other) noexcept;
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;
  ~Presenter();

  VkExtent2D extent() const { return extent_; }

  // Begins a frame. With a clear colour the target is cleared to it (raw sRGB-space components —
  // the target is UNORM, so nothing is encoded on write); with nullopt the target is handed over
  // undefined, which is what an app whose first pass covers every pixel wants: a full-screen clear
  // it immediately overwrites is a wasted write of width*height*4 bytes every frame.
  // Returns nullopt when the swapchain needs rebuilding — call resize() and retry.
  // pre: no frame is currently open.
  std::expected<std::optional<FrameTarget>, std::string> begin_frame(std::optional<glm::vec3> clear_color);

  // Ends recording, acquires a swapchain image and submits. On the same-GPU path the frame also
  // reaches the screen here; on the cross-GPU path it is queued and the *previous* frame is the one
  // handed over and presented. A swapchain that has gone out of date is not an error: the frame is
  // dropped (or presented, if merely suboptimal) and the next begin_frame() returns nullopt.
  // pre: begin_frame() returned a target.
  std::expected<void, std::string> end_frame();

  // True while a cross-GPU frame has been submitted but not yet presented. Same-GPU: always false.
  bool has_deferred_frame() const { return pending_slot_.has_value(); }

  // Rebuilds the swapchain and every size-dependent resource. pre: width/height > 0.
  std::expected<void, std::string> resize(uint32_t width, uint32_t height);

  // Switches present mode and rebuilds the swapchain (the mode is fixed at creation, so there is no
  // cheaper way). No-op when the preference is unchanged. The surface may not offer what was asked
  // for — present_mode_name() reports what is actually in use.
  // pre: no frame is currently open.
  std::expected<void, std::string> set_present_preference(PresentPreference pref);
  PresentPreference present_preference() const { return present_pref_; }
  const char* present_mode_name() const;  // e.g. "FIFO (vsync)", "MAILBOX", "IMMEDIATE"
  // How many images the swapchain actually got. Worth logging: MAILBOX with fewer than three
  // behaves like FIFO, so this is the number that says whether the mode can do anything.
  uint32_t swapchain_image_count() const { return static_cast<uint32_t>(swapchain_.images().size()); }

  // The most recently presented frame as tightly packed BGRA8 rows (extent().width * 4 per row).
  // Stalls both devices; intended for screenshots, not for streaming.
  std::expected<std::vector<uint8_t>, std::string> read_last_frame() const;

 private:
  std::expected<void, std::string> build_size_dependent(uint32_t width, uint32_t height);
  // Cross-GPU only: acquires an image, copies the pending frame's pixels across and presents it.
  // A no-op when nothing is pending. An out-of-date acquire drops the frame and arms needs_resize_.
  std::expected<void, std::string> flush_pending();
  // Shared tail of both paths: classifies an acquire/present result. Out-of-date and suboptimal arm
  // needs_resize_ rather than failing — only a genuine error becomes an unexpected.
  std::expected<bool, std::string> handle_swapchain_result(VkResult r, const char* what);
  void destroy() noexcept;
  bool cross_gpu() const;

  // Owned by the render device: where the app draws, plus the readback staging used to leave the GPU.
  struct RenderFrame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Image2D color;
    Buffer readback;  // cross-GPU only
  };

  // Owned by the present device: the upload staging and the command buffer that fills a swapchain
  // image. Only `image_available` is used on the same-GPU path.
  struct PresentFrame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer staging;  // cross-GPU only
    VkSemaphore image_available = VK_NULL_HANDLE;
  };

  const VulkanContext* ctx_ = nullptr;
  Swapchain swapchain_;
  VkExtent2D extent_{};
  PresentPreference present_pref_ = PresentPreference::Vsync;

  VkCommandPool render_pool_ = VK_NULL_HANDLE;
  VkCommandPool present_pool_ = VK_NULL_HANDLE;
  std::array<RenderFrame, kFramesInFlight> render_frames_;
  std::array<PresentFrame, kFramesInFlight> present_frames_;
  // One per swapchain image, not per frame in flight: present waits on it, so it must not be
  // reused while an older present on the same image is still pending.
  std::vector<VkSemaphore> render_finished_;

  uint32_t frame_index_ = 0;
  uint32_t image_index_ = 0;
  bool frame_open_ = false;
  // Cross-GPU only: the slot rendered last end_frame() and not yet handed to the present device.
  std::optional<uint32_t> pending_slot_;
  // A swapchain that acquire or present reported as out-of-date or suboptimal. Set on either path;
  // the next begin_frame() turns it into a nullopt so the caller resizes. Suboptimal counts: it is
  // presentable but wrong-sized, and ignoring it leaves the window scaling forever.
  bool needs_resize_ = false;
  // Set once a rebuild has come back at the very same extent, which means the surface is reporting
  // SUBOPTIMAL for something a rebuild cannot fix (fractional scaling, an unusual surface size).
  // Some compositors do that on every single present; without this, honouring them would rebuild
  // the swapchain every frame, which is far worse than running slightly mismatched. Cleared as soon
  // as a rebuild actually changes the extent.
  bool suboptimal_settled_ = false;
};

}  // namespace vkhb::render
