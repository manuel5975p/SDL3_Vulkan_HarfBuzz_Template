#include "present.hpp"

#include "vk_context.hpp"
#include "vk_error.hpp"

#include <cstring>
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

std::expected<VkCommandPool, std::string> make_pool(const Device& dev, uint32_t family) {
  const VkCommandPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                     .queueFamilyIndex = family};
  VkCommandPool pool = VK_NULL_HANDLE;
  if (const VkResult r = dev.vk().vkCreateCommandPool(dev.handle(), &info, nullptr, &pool); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateCommandPool", r));
  return pool;
}

std::expected<void, std::string> alloc_cmds(const Device& dev, VkCommandPool pool, VkCommandBuffer* out,
                                            uint32_t count) {
  const VkCommandBufferAllocateInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = count};
  if (const VkResult r = dev.vk().vkAllocateCommandBuffers(dev.handle(), &info, out); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkAllocateCommandBuffers", r));
  return {};
}

std::expected<VkFence, std::string> make_fence(const Device& dev) {
  const VkFenceCreateInfo info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
  VkFence fence = VK_NULL_HANDLE;
  if (const VkResult r = dev.vk().vkCreateFence(dev.handle(), &info, nullptr, &fence); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateFence", r));
  return fence;
}

std::expected<VkSemaphore, std::string> make_semaphore(const Device& dev) {
  const VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore sem = VK_NULL_HANDLE;
  if (const VkResult r = dev.vk().vkCreateSemaphore(dev.handle(), &info, nullptr, &sem); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkCreateSemaphore", r));
  return sem;
}

}  // namespace

bool Presenter::cross_gpu() const { return ctx_->cross_gpu(); }

std::expected<Presenter, std::string> Presenter::create(const VulkanContext& ctx, uint32_t width, uint32_t height) {
  Presenter p;
  p.ctx_ = &ctx;

  const Device& render = ctx.render();
  const Device& present = ctx.present();

  auto render_pool = make_pool(render, render.graphics_family());
  if (!render_pool) return std::unexpected(render_pool.error());
  p.render_pool_ = *render_pool;

  std::array<VkCommandBuffer, kFramesInFlight> render_cmds{};
  if (auto r = alloc_cmds(render, p.render_pool_, render_cmds.data(), kFramesInFlight); !r)
    return std::unexpected(r.error());

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    p.render_frames_[i].cmd = render_cmds[i];
    auto fence = make_fence(render);
    if (!fence) return std::unexpected(fence.error());
    p.render_frames_[i].fence = *fence;
  }

  // The present side always needs its own acquire semaphores; it needs command buffers, fences and
  // staging only when it is a different device that has to be fed from host memory.
  auto present_pool = make_pool(present, present.graphics_family());
  if (!present_pool) return std::unexpected(present_pool.error());
  p.present_pool_ = *present_pool;

  std::array<VkCommandBuffer, kFramesInFlight> present_cmds{};
  if (p.cross_gpu()) {
    if (auto r = alloc_cmds(present, p.present_pool_, present_cmds.data(), kFramesInFlight); !r)
      return std::unexpected(r.error());
  }
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    auto sem = make_semaphore(present);
    if (!sem) return std::unexpected(sem.error());
    p.present_frames_[i].image_available = *sem;
    if (p.cross_gpu()) {
      p.present_frames_[i].cmd = present_cmds[i];
      auto fence = make_fence(present);
      if (!fence) return std::unexpected(fence.error());
      p.present_frames_[i].fence = *fence;
    }
  }

  if (auto r = p.build_size_dependent(width, height); !r) return std::unexpected(r.error());
  return p;
}

std::expected<void, std::string> Presenter::build_size_dependent(uint32_t width, uint32_t height) {
  const Device& render = ctx_->render();
  const Device& present = ctx_->present();

  if (swapchain_.handle() == VK_NULL_HANDLE) {
    auto sc = Swapchain::create(*ctx_, width, height, present_pref_);
    if (!sc) return std::unexpected(sc.error());
    swapchain_ = std::move(*sc);
  } else if (auto r = swapchain_.recreate(*ctx_, width, height, present_pref_); !r) {
    return std::unexpected(r.error());
  }
  // A rebuild that lands on the same extent as before cannot have addressed a SUBOPTIMAL report,
  // so stop acting on those until the size genuinely changes (see suboptimal_settled_).
  const VkExtent2D previous = extent_;
  extent_ = swapchain_.extent();
  suboptimal_settled_ = previous.width == extent_.width && previous.height == extent_.height;

  // The offscreen target matches the swapchain extent exactly so the blit is a straight copy.
  const VkDeviceSize frame_bytes = VkDeviceSize{extent_.width} * extent_.height * 4;
  for (auto& frame : render_frames_) {
    // TRANSFER_DST as well as SRC: begin_frame() clears through vkCmdClearColorImage, which counts
    // as a transfer write, not an attachment clear.
    auto color = Image2D::create(render, extent_, kColorFormat,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
    if (!color) return std::unexpected(color.error());
    frame.color = std::move(*color);

    if (!cross_gpu()) continue;
    auto readback = Buffer::create(render, frame_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!readback) return std::unexpected(readback.error());
    frame.readback = std::move(*readback);
  }

  if (cross_gpu()) {
    for (auto& frame : present_frames_) {
      auto staging = Buffer::create(present, frame_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!staging) return std::unexpected(staging.error());
      frame.staging = std::move(*staging);
    }
  }

  for (VkSemaphore s : render_finished_) present.vk().vkDestroySemaphore(present.handle(), s, nullptr);
  render_finished_.clear();
  for (size_t i = 0; i < swapchain_.images().size(); ++i) {
    auto sem = make_semaphore(present);
    if (!sem) return std::unexpected(sem.error());
    render_finished_.push_back(*sem);
  }
  return {};
}

std::expected<bool, std::string> Presenter::handle_swapchain_result(VkResult r, const char* what) {
  // SUBOPTIMAL is deliberately treated like OUT_OF_DATE rather than ignored. The image is usable
  // and this frame still goes to the screen, but the swapchain no longer matches the surface — and
  // a compositor that reports it once reports it every frame, so ignoring it means running scaled
  // forever. Arming needs_resize_ costs one rebuild and settles.
  if (r == VK_SUBOPTIMAL_KHR) {
    if (!suboptimal_settled_) needs_resize_ = true;
    return true;  // usable either way — the frame still goes to the screen
  }
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    needs_resize_ = true;
    return false;  // nothing was acquired / the present did not happen
  }
  if (r != VK_SUCCESS) return std::unexpected(vk_error(what, r));
  return true;
}

std::expected<std::optional<FrameTarget>, std::string> Presenter::begin_frame(std::optional<glm::vec3> clear_color) {
  const Device& render = ctx_->render();
  RenderFrame& frame = render_frames_[frame_index_];

  // The render fence guards this slot's command buffer, colour target and readback buffer. On the
  // cross-GPU path flush_pending() already waited it out one end_frame() ago, so this rarely
  // blocks — which is the point: the CPU stays a frame ahead of the render GPU.
  render.vk().vkWaitForFences(render.handle(), 1, &frame.fence, VK_TRUE, UINT64_MAX);

  // Both paths report a stale swapchain here, before anything is reset, so the fence stays
  // signalled and the caller can resize() and come straight back.
  if (needs_resize_) {
    needs_resize_ = false;
    return std::nullopt;
  }

  render.vk().vkResetFences(render.handle(), 1, &frame.fence);

  const VolkDeviceTable& vk = render.vk();
  vk.vkResetCommandBuffer(frame.cmd, 0);
  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  if (const VkResult r = vk.vkBeginCommandBuffer(frame.cmd, &begin); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkBeginCommandBuffer", r));

  if (clear_color) {
    // Clear the offscreen target, then hand it over in COLOR_ATTACHMENT_OPTIMAL. Components are raw
    // sRGB-space values: the target is UNORM, so nothing is encoded on write.
    image_barrier(vk, frame.cmd, frame.color.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue clear{{clear_color->r, clear_color->g, clear_color->b, 1.0f}};
    const VkImageSubresourceRange whole{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
    vk.vkCmdClearColorImage(frame.cmd, frame.color.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &whole);
    image_barrier(vk, frame.cmd, frame.color.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
  } else {
    // No clear: straight from UNDEFINED (the previous contents are discarded either way) into the
    // attachment layout. The caller has promised its first pass writes every pixel.
    image_barrier(vk, frame.cmd, frame.color.image(), VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
  }

  frame_open_ = true;
  return FrameTarget{.cmd = frame.cmd,
                     .color_view = frame.color.view(),
                     .color_image = frame.color.image(),
                     .extent = extent_,
                     .frame_index = frame_index_};
}

std::expected<void, std::string> Presenter::end_frame() {
  const Device& render = ctx_->render();
  RenderFrame& frame = render_frames_[frame_index_];
  PresentFrame& pframe = present_frames_[frame_index_];
  const VolkDeviceTable& vk = render.vk();

  // The app is done drawing: the target becomes a transfer source either way.
  image_barrier(vk, frame.cmd, frame.color.image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);

  const VkImageSubresourceLayers layers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1};

  // Whether a swapchain image was acquired for this frame. False only when the swapchain went out
  // of date, in which case the command buffer is still submitted (without the copy and without any
  // semaphore) so that this slot's fence signals and the frame protocol stays intact.
  bool have_image = false;

  if (!cross_gpu()) {
    // The acquire happens here, not in begin_frame(): it blocks until the presentation engine
    // releases an image, and everything the app wanted to record has now been recorded, so that
    // wait overlaps the frame's CPU work instead of preceding it. The semaphore is free — the
    // submit that last waited on it is the one this slot's fence guarded in begin_frame().
    const Device& present_dev = ctx_->present();
    const VkResult acquire = present_dev.vk().vkAcquireNextImageKHR(
        present_dev.handle(), swapchain_.handle(), UINT64_MAX, pframe.image_available, VK_NULL_HANDLE, &image_index_);
    auto usable = handle_swapchain_result(acquire, "vkAcquireNextImageKHR");
    if (!usable) return std::unexpected(usable.error());
    have_image = *usable;

    if (have_image) {
      // Same device: copy straight into the acquired swapchain image. UNORM -> SRGB of equal texel
      // size is a size-compatible copy, i.e. a raw byte move with no colour conversion — which is
      // exactly what keeps the gamma-space blend intact.
      const VkImage swap_image = swapchain_.images()[image_index_];
      // srcStageMask is TRANSFER, matching the submit's wait stage below: the acquire semaphore
      // makes the submit's transfer work wait, so a barrier sourced at TRANSFER is ordered after it.
      // TOP_OF_PIPE would name no stage the wait covers, leaving the layout transition free to run
      // before the presentation engine had finished with the image.
      image_barrier(vk, frame.cmd, swap_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
      const VkImageCopy region{.srcSubresource = layers,
                               .dstSubresource = layers,
                               .extent = {extent_.width, extent_.height, 1}};
      vk.vkCmdCopyImage(frame.cmd, frame.color.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap_image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      image_barrier(vk, frame.cmd, swap_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);
    }
  } else {
    // Different device: the frame has to leave this GPU through host memory.
    const VkBufferImageCopy region{.imageSubresource = layers, .imageExtent = {extent_.width, extent_.height, 1}};
    vk.vkCmdCopyImageToBuffer(frame.cmd, frame.color.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              frame.readback.handle(), 1, &region);
  }

  if (const VkResult r = vk.vkEndCommandBuffer(frame.cmd); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkEndCommandBuffer", r));
  frame_open_ = false;

  if (cross_gpu()) {
    // Submit and move on. The handover of the frame *before* this one costs a CPU wait, but that
    // fence went signalled a whole frame ago, so it does not stall.
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &frame.cmd};
    if (const VkResult r = vk.vkQueueSubmit(render.graphics_queue(), 1, &submit, frame.fence); r != VK_SUCCESS)
      return std::unexpected(vk_error("vkQueueSubmit (render)", r));

    auto flushed = flush_pending();
    pending_slot_ = frame_index_;
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    return flushed;
  }

  const Device& present = ctx_->present();

  // Nothing to present into: submit bare so the fence still signals and this slot can be reused.
  // needs_resize_ is already armed, so the next begin_frame() sends the caller to resize().
  if (!have_image) {
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &frame.cmd};
    if (const VkResult r = vk.vkQueueSubmit(render.graphics_queue(), 1, &submit, frame.fence); r != VK_SUCCESS)
      return std::unexpected(vk_error("vkQueueSubmit (dropped frame)", r));
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    return {};
  }

  // Waiting at TRANSFER, not COLOR_ATTACHMENT_OUTPUT: the swapchain image is only ever touched by
  // the copy at the end of this command buffer, so the scene and UI passes can start before the
  // presentation engine has released it.
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkSubmitInfo submit{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &pframe.image_available,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &frame.cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &render_finished_[image_index_],
  };
  if (const VkResult r = vk.vkQueueSubmit(render.graphics_queue(), 1, &submit, frame.fence); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkQueueSubmit", r));

  const VkSwapchainKHR handle = swapchain_.handle();
  const VkPresentInfoKHR present_info{
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &render_finished_[image_index_],
      .swapchainCount = 1,
      .pSwapchains = &handle,
      .pImageIndices = &image_index_,
  };
  const VkResult r = present.vk().vkQueuePresentKHR(present.present_queue(), &present_info);
  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
  auto presented = handle_swapchain_result(r, "vkQueuePresentKHR");
  if (!presented) return std::unexpected(presented.error());
  return {};
}

std::expected<void, std::string> Presenter::flush_pending() {
  if (!pending_slot_) return {};
  const uint32_t slot = *pending_slot_;
  pending_slot_.reset();

  const Device& render = ctx_->render();
  const Device& present = ctx_->present();
  const VolkDeviceTable& pvk = present.vk();
  RenderFrame& frame = render_frames_[slot];
  PresentFrame& pframe = present_frames_[slot];

  // No semaphore can span two devices, so the CPU is the synchronisation point. Both waits are on
  // work submitted a full frame ago and are expected to be already signalled.
  render.vk().vkWaitForFences(render.handle(), 1, &frame.fence, VK_TRUE, UINT64_MAX);
  pvk.vkWaitForFences(present.handle(), 1, &pframe.fence, VK_TRUE, UINT64_MAX);

  // Acquire last: with FIFO this is where the loop blocks on vsync, which is exactly what makes the
  // present mode observable again. The semaphore is free — the submit that consumed it last is the
  // one pframe.fence just guarded.
  const VkResult acquire = pvk.vkAcquireNextImageKHR(present.handle(), swapchain_.handle(), UINT64_MAX,
                                                     pframe.image_available, VK_NULL_HANDLE, &image_index_);
  auto usable = handle_swapchain_result(acquire, "vkAcquireNextImageKHR");
  if (!usable) return std::unexpected(usable.error());
  if (!*usable) return {};  // out of date: drop this frame, the next begin_frame() asks for a resize
  pvk.vkResetFences(present.handle(), 1, &pframe.fence);

  std::memcpy(pframe.staging.mapped(), frame.readback.mapped(),
              static_cast<size_t>(extent_.width) * extent_.height * 4);

  const VkImage swap_image = swapchain_.images()[image_index_];
  const VkImageSubresourceLayers layers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1};
  pvk.vkResetCommandBuffer(pframe.cmd, 0);
  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  if (const VkResult r = pvk.vkBeginCommandBuffer(pframe.cmd, &begin); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkBeginCommandBuffer (present)", r));
  // Same ordering requirement as the same-GPU path: sourced at TRANSFER so the acquire semaphore's
  // wait (also at TRANSFER, below) sequences the layout transition after it.
  image_barrier(pvk, pframe.cmd, swap_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
  const VkBufferImageCopy region{.imageSubresource = layers, .imageExtent = {extent_.width, extent_.height, 1}};
  pvk.vkCmdCopyBufferToImage(pframe.cmd, pframe.staging.handle(), swap_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &region);
  image_barrier(pvk, pframe.cmd, swap_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);
  if (const VkResult r = pvk.vkEndCommandBuffer(pframe.cmd); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkEndCommandBuffer (present)", r));

  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkSubmitInfo psubmit{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &pframe.image_available,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &pframe.cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &render_finished_[image_index_],
  };
  if (const VkResult r = pvk.vkQueueSubmit(present.graphics_queue(), 1, &psubmit, pframe.fence); r != VK_SUCCESS)
    return std::unexpected(vk_error("vkQueueSubmit (present)", r));

  const VkSwapchainKHR handle = swapchain_.handle();
  const VkPresentInfoKHR present_info{
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &render_finished_[image_index_],
      .swapchainCount = 1,
      .pSwapchains = &handle,
      .pImageIndices = &image_index_,
  };
  const VkResult r = pvk.vkQueuePresentKHR(present.present_queue(), &present_info);
  auto presented = handle_swapchain_result(r, "vkQueuePresentKHR");
  if (!presented) return std::unexpected(presented.error());
  return {};
}

std::expected<void, std::string> Presenter::resize(uint32_t width, uint32_t height) {
  const Device& render = ctx_->render();
  const Device& present = ctx_->present();
  render.vk().vkDeviceWaitIdle(render.handle());
  if (cross_gpu()) present.vk().vkDeviceWaitIdle(present.handle());
  frame_open_ = false;
  // Every size-dependent resource below is about to be replaced, so a deferred frame still holding
  // the old target is dropped rather than presented at the wrong extent.
  pending_slot_.reset();
  needs_resize_ = false;
  return build_size_dependent(width, height);
}

std::expected<void, std::string> Presenter::set_present_preference(PresentPreference pref) {
  if (pref == present_pref_) return {};
  present_pref_ = pref;
  return resize(extent_.width, extent_.height);
}

const char* Presenter::present_mode_name() const {
  switch (swapchain_.present_mode()) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
    default: return "FIFO-like";
  }
}

std::expected<std::vector<uint8_t>, std::string> Presenter::read_last_frame() const {
  const Device& render = ctx_->render();
  const VolkDeviceTable& vk = render.vk();
  render.vk().vkDeviceWaitIdle(render.handle());

  // The most recent complete frame: the one still awaiting handover on the cross-GPU path, else the
  // one before frame_index_ (which end_frame() has already advanced past).
  const uint32_t last = pending_slot_.value_or((frame_index_ + kFramesInFlight - 1) % kFramesInFlight);
  const Image2D& color = render_frames_[last].color;
  const VkDeviceSize bytes = VkDeviceSize{extent_.width} * extent_.height * 4;

  auto staging = Buffer::create(render, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!staging) return std::unexpected(staging.error());

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (auto r = alloc_cmds(render, render_pool_, &cmd, 1); !r) return std::unexpected(r.error());

  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  vk.vkBeginCommandBuffer(cmd, &begin);
  // end_frame() left the target in TRANSFER_SRC_OPTIMAL, so it can be read as-is.
  const VkBufferImageCopy region{
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
      .imageExtent = {extent_.width, extent_.height, 1},
  };
  vk.vkCmdCopyImageToBuffer(cmd, color.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->handle(), 1, &region);
  vk.vkEndCommandBuffer(cmd);

  const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
  vk.vkQueueSubmit(render.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
  vk.vkQueueWaitIdle(render.graphics_queue());
  vk.vkFreeCommandBuffers(render.handle(), render_pool_, 1, &cmd);

  std::vector<uint8_t> pixels(static_cast<size_t>(bytes));
  staging->download(pixels.data(), bytes);
  return pixels;
}

void Presenter::destroy() noexcept {
  if (!ctx_) return;
  const Device& render = ctx_->render();
  const Device& present = ctx_->present();
  render.vk().vkDeviceWaitIdle(render.handle());
  if (cross_gpu()) present.vk().vkDeviceWaitIdle(present.handle());

  for (auto& frame : render_frames_) {
    if (frame.fence) render.vk().vkDestroyFence(render.handle(), frame.fence, nullptr);
    frame.color = Image2D{};
    frame.readback = Buffer{};
  }
  for (auto& frame : present_frames_) {
    if (frame.image_available) present.vk().vkDestroySemaphore(present.handle(), frame.image_available, nullptr);
    if (frame.fence) present.vk().vkDestroyFence(present.handle(), frame.fence, nullptr);
    frame.staging = Buffer{};
  }
  for (VkSemaphore s : render_finished_) present.vk().vkDestroySemaphore(present.handle(), s, nullptr);
  render_finished_.clear();

  swapchain_ = Swapchain{};
  if (render_pool_) render.vk().vkDestroyCommandPool(render.handle(), render_pool_, nullptr);
  if (present_pool_) present.vk().vkDestroyCommandPool(present.handle(), present_pool_, nullptr);
  render_pool_ = VK_NULL_HANDLE;
  present_pool_ = VK_NULL_HANDLE;
  ctx_ = nullptr;
}

Presenter::Presenter(Presenter&& other) noexcept
    : ctx_(std::exchange(other.ctx_, nullptr)),
      swapchain_(std::move(other.swapchain_)),
      extent_(std::exchange(other.extent_, VkExtent2D{})),
      render_pool_(std::exchange(other.render_pool_, VK_NULL_HANDLE)),
      present_pool_(std::exchange(other.present_pool_, VK_NULL_HANDLE)),
      render_frames_(std::move(other.render_frames_)),
      present_frames_(std::move(other.present_frames_)),
      render_finished_(std::move(other.render_finished_)),
      frame_index_(std::exchange(other.frame_index_, 0)),
      image_index_(std::exchange(other.image_index_, 0)),
      frame_open_(std::exchange(other.frame_open_, false)),
      pending_slot_(std::exchange(other.pending_slot_, std::nullopt)),
      needs_resize_(std::exchange(other.needs_resize_, false)),
      suboptimal_settled_(std::exchange(other.suboptimal_settled_, false)) {
  for (auto& f : other.render_frames_) f = RenderFrame{};
  for (auto& f : other.present_frames_) f = PresentFrame{};
}

Presenter& Presenter::operator=(Presenter&& other) noexcept {
  if (this == &other) return *this;
  destroy();
  ctx_ = std::exchange(other.ctx_, nullptr);
  swapchain_ = std::move(other.swapchain_);
  extent_ = std::exchange(other.extent_, VkExtent2D{});
  render_pool_ = std::exchange(other.render_pool_, VK_NULL_HANDLE);
  present_pool_ = std::exchange(other.present_pool_, VK_NULL_HANDLE);
  render_frames_ = std::move(other.render_frames_);
  present_frames_ = std::move(other.present_frames_);
  render_finished_ = std::move(other.render_finished_);
  frame_index_ = std::exchange(other.frame_index_, 0);
  image_index_ = std::exchange(other.image_index_, 0);
  frame_open_ = std::exchange(other.frame_open_, false);
  pending_slot_ = std::exchange(other.pending_slot_, std::nullopt);
  needs_resize_ = std::exchange(other.needs_resize_, false);
  suboptimal_settled_ = std::exchange(other.suboptimal_settled_, false);
  for (auto& f : other.render_frames_) f = RenderFrame{};
  for (auto& f : other.present_frames_) f = PresentFrame{};
  return *this;
}

Presenter::~Presenter() { destroy(); }

}  // namespace vkhb::render
