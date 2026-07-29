#include "vk_buffer.hpp"

#include "vk_context.hpp"
#include "vk_error.hpp"

#include <cstddef>
#include <cstring>
#include <utility>

namespace vkhb::render {

std::expected<Buffer, std::string> Buffer::create(const Device& dev, VkDeviceSize size, VkBufferUsageFlags usage,
                                                  VkMemoryPropertyFlags properties) {
  const VolkDeviceTable& vk = dev.vk();
  const VkDevice device = dev.handle();

  Buffer buf;
  buf.vk_ = &vk;
  buf.device_ = device;
  buf.size_ = size;
  buf.host_visible_ = properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  const VkBufferCreateInfo buffer_info{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (const VkResult r = vk.vkCreateBuffer(device, &buffer_info, nullptr, &buf.buffer_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateBuffer", r));

  VkMemoryRequirements reqs;
  vk.vkGetBufferMemoryRequirements(device, buf.buffer_, &reqs);

  // From here on `buf` owns the handles it has so far, so an early return unwinds through ~Buffer().
  const auto mem_type = dev.find_memory_type(reqs.memoryTypeBits, properties);
  if (!mem_type) return std::unexpected(mem_type.error());

  const VkMemoryAllocateInfo alloc_info{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size,
      .memoryTypeIndex = *mem_type,
  };
  if (const VkResult r = vk.vkAllocateMemory(device, &alloc_info, nullptr, &buf.memory_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkAllocateMemory", r));
  vk.vkBindBufferMemory(device, buf.buffer_, buf.memory_, 0);

  if (buf.host_visible_) {
    if (const VkResult r = vk.vkMapMemory(device, buf.memory_, 0, VK_WHOLE_SIZE, 0, &buf.mapped_); r != VK_SUCCESS)
      return std::unexpected(vk_error("vkMapMemory", r));
  }
  return buf;
}

void Buffer::upload(const void* data, VkDeviceSize size) const { upload_at(0, data, size); }

void Buffer::upload_at(VkDeviceSize offset, const void* data, VkDeviceSize size) const {
  std::memcpy(static_cast<std::byte*>(mapped_) + offset, data, static_cast<size_t>(size));
}

void Buffer::download(void* data, VkDeviceSize size) const {
  std::memcpy(data, mapped_, static_cast<size_t>(size));
}

void Buffer::destroy() noexcept {
  if (mapped_) vk_->vkUnmapMemory(device_, memory_);
  if (memory_) vk_->vkFreeMemory(device_, memory_, nullptr);
  mapped_ = nullptr;
  if (buffer_) vk_->vkDestroyBuffer(device_, buffer_, nullptr);
  buffer_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
}

Buffer::Buffer(Buffer&& other) noexcept
    : vk_(std::exchange(other.vk_, nullptr)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      memory_(std::exchange(other.memory_, VK_NULL_HANDLE)),
      mapped_(std::exchange(other.mapped_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      host_visible_(std::exchange(other.host_visible_, false)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  vk_ = std::exchange(other.vk_, nullptr);
  device_ = std::exchange(other.device_, VK_NULL_HANDLE);
  buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
  memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
  mapped_ = std::exchange(other.mapped_, nullptr);
  size_ = std::exchange(other.size_, 0);
  host_visible_ = std::exchange(other.host_visible_, false);
  return *this;
}

Buffer::~Buffer() { destroy(); }

}  // namespace vkhb::render
