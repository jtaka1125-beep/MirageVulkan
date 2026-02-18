#!/usr/bin/env python3
"""Patch MirageVulkan gui_application.cpp to fix GUI freeze issues"""

filepath = r'C:\MirageWork\MirageVulkan\src\gui_application.cpp'

with open(filepath, 'r', encoding='utf-8-sig') as f:
    content = f.read()

# === FIX 1: vulkanBeginFrame - fence timeout + resizing guard ===
old_begin = '''void GuiApplication::vulkanBeginFrame() {
    frame_valid_ = false;

    VkDevice dev = vk_context_->device();
    uint32_t fi = vk_current_frame_;

    vkWaitForFences(dev, 1, &vk_in_flight_[fi], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &vk_in_flight_[fi]);

    uint32_t imageIndex;
    VkResult r = vkAcquireNextImageKHR(dev, vk_swapchain_->swapchain(),
        UINT64_MAX, vk_image_available_[fi], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        MLOG_INFO("vkframe", "acquire OUT_OF_DATE, recreating");
        vk_swapchain_->recreate(window_width_, window_height_);
        return;
    }'''

new_begin = '''void GuiApplication::vulkanBeginFrame() {
    frame_valid_ = false;

    // Skip frames during window resize to prevent Vulkan errors
    if (resizing_.load()) return;

    VkDevice dev = vk_context_->device();
    uint32_t fi = vk_current_frame_;

    // Use 3-second timeout instead of UINT64_MAX to prevent permanent freeze
    VkResult fence_r = vkWaitForFences(dev, 1, &vk_in_flight_[fi], VK_TRUE, 3000000000ULL);
    if (fence_r == VK_TIMEOUT) {
        MLOG_WARN("vkframe", "Fence timeout (3s), recovering...");
        vkDeviceWaitIdle(dev);
        // Recreate ALL fences in signaled state to break deadlock
        for (uint32_t i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroyFence(dev, vk_in_flight_[i], nullptr);
            VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(dev, &fci, nullptr, &vk_in_flight_[i]);
        }
        return;  // Skip this frame, next iteration will succeed
    }
    if (fence_r != VK_SUCCESS) {
        MLOG_ERROR("vkframe", "Fence wait error: %d", (int)fence_r);
        return;
    }
    vkResetFences(dev, 1, &vk_in_flight_[fi]);

    uint32_t imageIndex;
    VkResult r = vkAcquireNextImageKHR(dev, vk_swapchain_->swapchain(),
        1000000000ULL, vk_image_available_[fi], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        MLOG_INFO("vkframe", "acquire OUT_OF_DATE/SUBOPTIMAL (%d), recreating", (int)r);
        vkDeviceWaitIdle(dev);
        vk_swapchain_->recreate(window_width_, window_height_);
        return;
    }
    if (r != VK_SUCCESS) {
        MLOG_ERROR("vkframe", "acquire failed: %d", (int)r);
        return;
    }'''

assert old_begin in content, "FIX1: old beginFrame block not found!"
content = content.replace(old_begin, new_begin)
print("FIX 1 applied: fence timeout + resizing guard in vulkanBeginFrame")

# === FIX 2: vulkanEndFrame - frame_valid guard + submit error handling ===
old_end = '''void GuiApplication::vulkanEndFrame() {
    static std::atomic<int> end_frame_count{0};
    int efc = end_frame_count.fetch_add(1);

    uint32_t fi = vk_current_frame_;
    VkCommandBuffer cmd = vk_command_buffers_[fi];

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &vk_image_available_[fi];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &vk_render_finished_[fi];
    VkResult sr = vkQueueSubmit(vk_context_->graphicsQueue(), 1, &si, vk_in_flight_[fi]);

    VkSwapchainKHR sc = vk_swapchain_->swapchain();
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk_render_finished_[fi];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &vk_current_image_index_;
    // Freeze diagnostics: present counters
    present_count_.fetch_add(1, std::memory_order_relaxed);
    last_present_ms_.store(getCurrentTimeMs(), std::memory_order_relaxed);

    VkResult r = vkQueuePresentKHR(vk_context_->graphicsQueue(), &pi);

    if (efc < 20 || (efc % 300 == 0)) {
        MLOG_INFO("vkframe", "endFrame #%d fi=%u img=%u submit=%d present=%d extent=%ux%u",
                  efc, fi, vk_current_image_index_, (int)sr, (int)r,
                  vk_swapchain_->extent().width, vk_swapchain_->extent().height);
    }

    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        MLOG_INFO("vkframe", "present OUT_OF_DATE, recreating");
        vk_swapchain_->recreate(window_width_, window_height_);
    }

    vk_current_frame_ = (vk_current_frame_ + 1) % VK_MAX_FRAMES_IN_FLIGHT;
}'''

new_end = '''void GuiApplication::vulkanEndFrame() {
    static std::atomic<int> end_frame_count{0};
    int efc = end_frame_count.fetch_add(1);

    // Guard: if beginFrame failed (frame_valid_ is false), skip rendering
    if (!frame_valid_) {
        vk_current_frame_ = (vk_current_frame_ + 1) % VK_MAX_FRAMES_IN_FLIGHT;
        return;
    }

    VkDevice dev = vk_context_->device();
    uint32_t fi = vk_current_frame_;
    VkCommandBuffer cmd = vk_command_buffers_[fi];

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &vk_image_available_[fi];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &vk_render_finished_[fi];
    VkResult sr = vkQueueSubmit(vk_context_->graphicsQueue(), 1, &si, vk_in_flight_[fi]);

    // If submit failed, don't try to present - it would use invalid semaphores
    if (sr != VK_SUCCESS) {
        MLOG_ERROR("vkframe", "submit FAILED: %d, skipping present", (int)sr);
        vkDeviceWaitIdle(dev);
        vk_current_frame_ = (vk_current_frame_ + 1) % VK_MAX_FRAMES_IN_FLIGHT;
        return;
    }

    VkSwapchainKHR sc = vk_swapchain_->swapchain();
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk_render_finished_[fi];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &vk_current_image_index_;
    // Freeze diagnostics: present counters
    present_count_.fetch_add(1, std::memory_order_relaxed);
    last_present_ms_.store(getCurrentTimeMs(), std::memory_order_relaxed);

    VkResult r = vkQueuePresentKHR(vk_context_->graphicsQueue(), &pi);

    if (efc < 20 || (efc % 300 == 0)) {
        MLOG_INFO("vkframe", "endFrame #%d fi=%u img=%u submit=%d present=%d extent=%ux%u",
                  efc, fi, vk_current_image_index_, (int)sr, (int)r,
                  vk_swapchain_->extent().width, vk_swapchain_->extent().height);
    }

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        MLOG_INFO("vkframe", "present OUT_OF_DATE/SUBOPTIMAL (%d), recreating", (int)r);
        vkDeviceWaitIdle(dev);
        vk_swapchain_->recreate(window_width_, window_height_);
    }

    vk_current_frame_ = (vk_current_frame_ + 1) % VK_MAX_FRAMES_IN_FLIGHT;
}'''

assert old_end in content, "FIX2: old endFrame block not found!"
content = content.replace(old_end, new_end)
print("FIX 2 applied: frame_valid guard + submit error handling in vulkanEndFrame")

with open(filepath, 'w', encoding='utf-8-sig') as f:
    f.write(content)

print("\nAll fixes written to", filepath)
