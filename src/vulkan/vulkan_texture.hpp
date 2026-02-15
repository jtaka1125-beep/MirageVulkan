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

    int width_ = 0, height_ = 0;
    bool layout_initialized_ = false;

    // Cached command buffer for update() reuse (avoids per-frame alloc/free)
    VkCommandBuffer cached_cmd_ = VK_NULL_HANDLE;
    VkCommandPool   cached_cmd_pool_ = VK_NULL_HANDLE;
};

} // namespace mirage::vk
