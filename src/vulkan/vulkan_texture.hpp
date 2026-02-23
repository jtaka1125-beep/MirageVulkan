#pragma once

#include "vulkan_context.hpp"

namespace mirage::vk {

class VulkanTexture {
public:
    VulkanTexture() = default;
    ~VulkanTexture() { destroy(); }

    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    bool create(VulkanContext& ctx, VkDescriptorPool pool, int width, int height);
    void update(VkCommandPool cmd_pool, VkQueue queue, const uint8_t* rgba, int width, int height);
    // Integrated path: stageUpdate() copies to staging (CPU only).
    // recordUpdate() records upload commands into external VkCommandBuffer (no separate submit).
    bool stageUpdate(const uint8_t* rgba, int w, int h);
    bool recordUpdate(VkCommandBuffer cmd);
    // Initialize/clear texture to a known color (prevents showing uninitialized VRAM)
    void clear(VkCommandPool cmd_pool, VkQueue queue, uint32_t rgba = 0xFF000000u);
    void destroy();

    VkDescriptorSet imguiDescriptorSet() const { return imgui_ds_; }
    int width()  const { return width_; }
    int height() const { return height_; }
    bool valid()  const { return image_ != VK_NULL_HANDLE; }

private:
    VulkanContext* ctx_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet imgui_ds_ = VK_NULL_HANDLE;

    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
    VkDeviceSize staging_size_ = 0;
    void* staging_mapped_ = nullptr; // persistently mapped (HOST_COHERENT)

    int width_ = 0, height_ = 0;
    bool layout_initialized_ = false;

    // Cached command buffer for update() reuse (avoids per-frame alloc/free)
    VkCommandBuffer cached_cmd_ = VK_NULL_HANDLE;
    VkCommandPool   cached_cmd_pool_ = VK_NULL_HANDLE;

    // Sync: fence for the last upload so we don't vkQueueWaitIdle() every frame.
    VkFence upload_fence_ = VK_NULL_HANDLE;

    // Diagnostics / robustness
    uint64_t last_submit_ms_ = 0;
    uint32_t skipped_updates_ = 0;
    uint32_t update_count_ = 0;
    bool has_pending_upload_ = false;
};

} // namespace mirage::vk
