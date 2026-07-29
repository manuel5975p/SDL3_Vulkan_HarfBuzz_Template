#pragma once

// Dedicated-allocation buffer helper (no suballocator): fine while buffer counts stay in the
// dozens, not the thousands where a real allocator (VMA) starts to matter.

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace vkhb::render {

class Device;

// Owns a VkBuffer + its dedicated VkDeviceMemory. Move-only.
class Buffer {
 public:
  // pre: dev outlives every Buffer made from it — the device table pointer is cached, not copied.
  static std::expected<Buffer, std::string> create(const Device& dev, VkDeviceSize size, VkBufferUsageFlags usage,
                                                   VkMemoryPropertyFlags properties);

  Buffer() = default;
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  ~Buffer();

  VkBuffer handle() const { return buffer_; }
  VkDeviceSize size() const { return size_; }

  // Persistently mapped memory, or nullptr if the buffer is not host-visible. Host-visible buffers
  // are mapped once at creation and stay mapped: per-frame staging would otherwise pay for a
  // map/unmap pair every upload.
  void* mapped() const { return mapped_; }

  // Copies `size` bytes from `data` into the buffer's memory.
  // pre: created with VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT; size <= this->size().
  void upload(const void* data, VkDeviceSize size) const;

  // upload() at an arbitrary byte offset (e.g. an append-only bump allocator).
  // pre: same as upload(); offset + size <= this->size().
  void upload_at(VkDeviceSize offset, const void* data, VkDeviceSize size) const;

  // Copies `size` bytes out of the buffer's memory (e.g. a readback buffer).
  // pre: same as upload(); the caller has already fenced/waited on the GPU write — this
  //      synchronizes nothing itself.
  void download(void* data, VkDeviceSize size) const;

 private:
  void destroy() noexcept;

  const VolkDeviceTable* vk_ = nullptr;
  VkDevice device_ = VK_NULL_HANDLE;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  void* mapped_ = nullptr;
  VkDeviceSize size_ = 0;
  bool host_visible_ = false;
};

}  // namespace vkhb::render
