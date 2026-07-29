#pragma once

// Windowless rendering: an offscreen colour target with the same begin/end/read frame shape as
// Presenter, so an app can record the exact same frame into either. Single-buffered and
// CPU-synchronised per frame: built for screenshot/CI runs, not for interactive frame rates.
// The surfaceless device it draws on comes from VulkanContext::create(nullptr, ...).

#include "vk_buffer.hpp"
#include "vk_context.hpp"
#include "vk_image.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

#include <volk.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace vkhb::render {

struct FrameTarget;

// The offscreen frame loop. Uses Presenter::kColorFormat-compatible B8G8R8A8_UNORM so the same
// pipelines draw into it.
class HeadlessTarget {
 public:
  static constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM;

  // pre: dev outlives the target; width/height > 0.
  static std::expected<HeadlessTarget, std::string> create(const Device& dev, uint32_t width, uint32_t height);

  HeadlessTarget() = default;
  HeadlessTarget(HeadlessTarget&& other) noexcept;
  HeadlessTarget& operator=(HeadlessTarget&& other) noexcept;
  HeadlessTarget(const HeadlessTarget&) = delete;
  HeadlessTarget& operator=(const HeadlessTarget&) = delete;
  ~HeadlessTarget();

  VkExtent2D extent() const { return extent_; }

  // Begins recording and clears the target, mirroring Presenter::begin_frame().
  // pre: no frame is currently open.
  std::expected<FrameTarget, std::string> begin_frame(glm::vec3 clear_color);

  // Submits and waits for completion (CPU-synchronised — frame_index is always 0).
  // pre: begin_frame() succeeded.
  std::expected<void, std::string> end_frame();

  // The last completed frame as tightly packed BGRA8 rows. pre: end_frame() succeeded at least once.
  std::expected<std::vector<uint8_t>, std::string> read_frame() const;

 private:
  void destroy() noexcept;

  const Device* dev_ = nullptr;
  VkExtent2D extent_{};
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  Image2D color_;
  Buffer readback_;
  bool frame_open_ = false;
};

}  // namespace vkhb::render
