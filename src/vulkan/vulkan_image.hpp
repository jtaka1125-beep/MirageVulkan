#pragma once

#include "vulkan_context.hpp"

namespace mirage::vk {

/**
 * GPU Image for Vulkan Compute pipelines.
 * 
 * Supports STORAGE and SAMPLED usage for compute shader I/O.
 * Can upload from CPU, download to CPU, and transition layouts.
 * Uses persistently mapped staging buffer for minimal transfer overhead.
 */
class VulkanImage {
public:
    VulkanImage() = default;
    ~VulkanImage() { destroy(); }

    VulkanImage(const VulkanImage&) = delete;
    VulkanImage& operator=(const VulkanImage&) = delete;

    bool create(VulkanContext& ctx, uint32_t width, uint32_t height,
                VkFormat format, VkImageUsageFlags extra_usage = 0);

    void destroy();

    bool upload(VkCommandPool cmd_pool, VkQueue queue,
                const void* data, size_t size);

    bool download(VkCommandPool cmd_pool, VkQueue queue,
                  void* out_data, size_t size);

    void transitionLayout(VkCommandBuffer cmd,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);

    // Direct staging access for zero-copy upload
    void* stagingPtr() const { return staging_mapped_; }
    VkDeviceSize stagingSize() const { return staging_size_; }

    // Submit pre-filled staging buffer (skip memcpy when data is already in staging)
    bool uploadFromStaging(VkCommandPool cmd_pool, VkQueue queue);

    // Accessors
    VkImage       image()     const { return image_; }
    VkImageView   imageView() const { return view_; }
    VkFormat      format()    const { return format_; }
    uint32_t      width()     const { return width_; }
    uint32_t      height()    const { return height_; }
    bool          valid()     const { return image_ != VK_NULL_HANDLE; }

private:
    bool createStagingBuffer(VkDeviceSize size);

    VulkanContext* ctx_ = nullptr;

    VkImage        image_   = VK_NULL_HANDLE;
    VkDeviceMemory memory_  = VK_NULL_HANDLE;
    VkImageView    view_    = VK_NULL_HANDLE;
    VkFormat       format_  = VK_FORMAT_UNDEFINED;
    uint32_t       width_   = 0;
    uint32_t       height_  = 0;

    // Staging buffer (persistently mapped)
    VkBuffer       staging_       = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_   = VK_NULL_HANDLE;
    VkDeviceSize   staging_size_  = 0;
    void*          staging_mapped_= nullptr;  // persistent map pointer
};

} // namespace mirage::vk
