#include "headless.hpp"

#include "present.hpp"
#include "vk_error.hpp"

#include <utility>

namespace vkhb::render {

namespace {

void image_barrier(const VolkDeviceTable& vk, VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access) {
  const VkImageMemoryBarrier2 barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
  };
  const VkDependencyInfo dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
  vk.vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

// --- HeadlessTarget ---

std::expected<HeadlessTarget, std::string> HeadlessTarget::create(const Device& dev, uint32_t width, uint32_t height) {
  HeadlessTarget t;
  t.dev_ = &dev;
  t.extent_ = {width, height};
  const VolkDeviceTable& vk = dev.vk();

  const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = dev.graphics_family()};
  if (const VkResult r = vk.vkCreateCommandPool(dev.handle(), &pool_info, nullptr, &t.pool_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateCommandPool (headless)", r));

  const VkCommandBufferAllocateInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = t.pool_,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
  if (const VkResult r = vk.vkAllocateCommandBuffers(dev.handle(), &cmd_info, &t.cmd_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkAllocateCommandBuffers (headless)", r));

  const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (const VkResult r = vk.vkCreateFence(dev.handle(), &fence_info, nullptr, &t.fence_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateFence (headless)", r));

  auto color = Image2D::create(dev, t.extent_, kColorFormat,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
  if (!color) return std::unexpected(color.error());
  t.color_ = std::move(*color);

  const VkDeviceSize bytes = VkDeviceSize{width} * height * 4;
  auto readback = Buffer::create(dev, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!readback) return std::unexpected(readback.error());
  t.readback_ = std::move(*readback);
  return t;
}

std::expected<FrameTarget, std::string> HeadlessTarget::begin_frame(glm::vec3 clear_color) {
  const VolkDeviceTable& vk = dev_->vk();
  vk.vkResetCommandBuffer(cmd_, 0);
  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  if (const VkResult r = vk.vkBeginCommandBuffer(cmd_, &begin); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkBeginCommandBuffer (headless)", r));

  image_barrier(vk, cmd_, color_.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
  const VkClearColorValue clear{{clear_color.r, clear_color.g, clear_color.b, 1.0f}};
  const VkImageSubresourceRange whole{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
  vk.vkCmdClearColorImage(cmd_, color_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &whole);
  image_barrier(vk, cmd_, color_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

  frame_open_ = true;
  return FrameTarget{
      .cmd = cmd_, .color_view = color_.view(), .color_image = color_.image(), .extent = extent_, .frame_index = 0};
}

std::expected<void, std::string> HeadlessTarget::end_frame() {
  const VolkDeviceTable& vk = dev_->vk();

  // Copy into the readback buffer inside the same submission, so read_frame() is a plain download.
  image_barrier(vk, cmd_, color_.image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
  const VkBufferImageCopy region{
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
      .imageExtent = {extent_.width, extent_.height, 1},
  };
  vk.vkCmdCopyImageToBuffer(cmd_, color_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.handle(), 1,
                            &region);
  if (const VkResult r = vk.vkEndCommandBuffer(cmd_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkEndCommandBuffer (headless)", r));
  frame_open_ = false;

  const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd_};
  if (const VkResult r = vk.vkQueueSubmit(dev_->graphics_queue(), 1, &submit, fence_); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkQueueSubmit (headless)", r));
  vk.vkWaitForFences(dev_->handle(), 1, &fence_, VK_TRUE, UINT64_MAX);
  vk.vkResetFences(dev_->handle(), 1, &fence_);
  return {};
}

std::expected<std::vector<uint8_t>, std::string> HeadlessTarget::read_frame() const {
  const VkDeviceSize bytes = VkDeviceSize{extent_.width} * extent_.height * 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(bytes));
  readback_.download(pixels.data(), bytes);
  return pixels;
}

void HeadlessTarget::destroy() noexcept {
  if (!dev_) return;
  const VolkDeviceTable& vk = dev_->vk();
  vk.vkDeviceWaitIdle(dev_->handle());
  color_ = Image2D{};
  readback_ = Buffer{};
  if (fence_) vk.vkDestroyFence(dev_->handle(), fence_, nullptr);
  if (pool_) vk.vkDestroyCommandPool(dev_->handle(), pool_, nullptr);
  fence_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  cmd_ = VK_NULL_HANDLE;
  dev_ = nullptr;
}

HeadlessTarget::HeadlessTarget(HeadlessTarget&& other) noexcept
    : dev_(std::exchange(other.dev_, nullptr)),
      extent_(std::exchange(other.extent_, VkExtent2D{})),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      cmd_(std::exchange(other.cmd_, VK_NULL_HANDLE)),
      fence_(std::exchange(other.fence_, VK_NULL_HANDLE)),
      color_(std::move(other.color_)),
      readback_(std::move(other.readback_)),
      frame_open_(std::exchange(other.frame_open_, false)) {}

HeadlessTarget& HeadlessTarget::operator=(HeadlessTarget&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  dev_ = std::exchange(other.dev_, nullptr);
  extent_ = std::exchange(other.extent_, VkExtent2D{});
  pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
  cmd_ = std::exchange(other.cmd_, VK_NULL_HANDLE);
  fence_ = std::exchange(other.fence_, VK_NULL_HANDLE);
  color_ = std::move(other.color_);
  readback_ = std::move(other.readback_);
  frame_open_ = std::exchange(other.frame_open_, false);
  return *this;
}

HeadlessTarget::~HeadlessTarget() { destroy(); }

}  // namespace vkhb::render
