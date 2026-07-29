#include "vk_image.hpp"

#include "vk_context.hpp"
#include "vk_error.hpp"

#include <utility>

namespace vkhb::render {

std::expected<Image2D, std::string> Image2D::create(const Device& dev, VkExtent2D extent, VkFormat format,
                                                    VkImageUsageFlags usage, VkImageAspectFlags aspect) {
  const VolkDeviceTable& vk = dev.vk();
  const VkDevice device = dev.handle();

  Image2D img;
  img.vk_ = &vk;
  img.device_ = device;
  img.format_ = format;
  img.extent_ = extent;

  const VkImageCreateInfo image_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {extent.width, extent.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (const VkResult r = vk.vkCreateImage(device, &image_info, nullptr, &img.image_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateImage", r));

  VkMemoryRequirements reqs;
  vk.vkGetImageMemoryRequirements(device, img.image_, &reqs);
  // From here on `img` owns the handles it has so far, so an early return unwinds through ~Image2D().
  const auto mem_type = dev.find_memory_type(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!mem_type) return std::unexpected(mem_type.error());
  const VkMemoryAllocateInfo alloc_info{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = reqs.size, .memoryTypeIndex = *mem_type};
  if (const VkResult r = vk.vkAllocateMemory(device, &alloc_info, nullptr, &img.memory_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkAllocateMemory", r));
  vk.vkBindImageMemory(device, img.image_, img.memory_, 0);

  const VkImageViewCreateInfo view_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img.image_,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = {.aspectMask = aspect, .levelCount = 1, .layerCount = 1},
  };
  if (const VkResult r = vk.vkCreateImageView(device, &view_info, nullptr, &img.view_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateImageView", r));
  return img;
}

void Image2D::destroy() noexcept {
  if (view_) vk_->vkDestroyImageView(device_, view_, nullptr);
  if (memory_) vk_->vkFreeMemory(device_, memory_, nullptr);
  if (image_) vk_->vkDestroyImage(device_, image_, nullptr);
  view_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
  image_ = VK_NULL_HANDLE;
}

Image2D::Image2D(Image2D&& other) noexcept
    : vk_(std::exchange(other.vk_, nullptr)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      memory_(std::exchange(other.memory_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)),
      format_(std::exchange(other.format_, VK_FORMAT_UNDEFINED)),
      extent_(std::exchange(other.extent_, VkExtent2D{})) {}

Image2D& Image2D::operator=(Image2D&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  vk_ = std::exchange(other.vk_, nullptr);
  device_ = std::exchange(other.device_, VK_NULL_HANDLE);
  image_ = std::exchange(other.image_, VK_NULL_HANDLE);
  memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
  view_ = std::exchange(other.view_, VK_NULL_HANDLE);
  format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
  extent_ = std::exchange(other.extent_, VkExtent2D{});
  return *this;
}

Image2D::~Image2D() { destroy(); }

}  // namespace vkhb::render
