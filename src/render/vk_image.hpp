#pragma once

// Device-local 2D image + view (depth buffer, shadow map) — same dedicated-allocation philosophy
// as vk_buffer.hpp.

#include <volk.h>

#include <expected>
#include <string>

namespace vkhb::render {

class Device;

class Image2D {
 public:
  // pre: dev outlives every Image2D made from it — the device table pointer is cached, not copied.
  static std::expected<Image2D, std::string> create(const Device& dev, VkExtent2D extent, VkFormat format,
                                                    VkImageUsageFlags usage, VkImageAspectFlags aspect);

  Image2D() = default;
  Image2D(Image2D&& other) noexcept;
  Image2D& operator=(Image2D&& other) noexcept;
  Image2D(const Image2D&) = delete;
  Image2D& operator=(const Image2D&) = delete;
  ~Image2D();

  VkImage image() const { return image_; }
  VkImageView view() const { return view_; }
  VkFormat format() const { return format_; }
  VkExtent2D extent() const { return extent_; }

 private:
  void destroy() noexcept;

  const VolkDeviceTable* vk_ = nullptr;
  VkDevice device_ = VK_NULL_HANDLE;
  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
};

}  // namespace vkhb::render
