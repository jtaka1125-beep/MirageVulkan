#include "vulkan_image.hpp"
#include "../mirage_log.hpp"
#include <cstring>

namespace mirage::vk {

bool VulkanImage::create(VulkanContext& ctx, uint32_t w, uint32_t h,
                          VkFormat format, VkImageUsageFlags extra_usage) {
    ctx_ = &ctx;
    width_ = w; height_ = h; format_ = format;
    VkDevice dev = ctx_->device();

    // Image (always STORAGE + TRANSFER_SRC + TRANSFER_DST)
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               extra_usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(dev, &ii, nullptr, &image_) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "createImage failed (%dx%d fmt=%d)", w, h, format);
        return false;
    }

    // Allocate device-local memory
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image_, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx_->findMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        MLOG_ERROR("VkImg", "No suitable device-local memory");
        return false;
    }

    if (vkAllocateMemory(dev, &ai, nullptr, &memory_) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "allocMemory failed");
        return false;
    }
    if (vkBindImageMemory(dev, image_, memory_, 0) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "vkBindImageMemory failed");
        return false;
    }

    // ImageView
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(dev, &vi, nullptr, &view_) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "createImageView failed");
        return false;
    }

    // Compute pixel size for staging
    uint32_t pixel_bytes = 0;
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: pixel_bytes = 4; break;
        case VK_FORMAT_R8_UNORM:       pixel_bytes = 1; break;
        case VK_FORMAT_R32_SFLOAT:     pixel_bytes = 4; break;
        case VK_FORMAT_R16_SFLOAT:     pixel_bytes = 2; break;
        default:
            MLOG_WARN("VkImg", "Unknown format %d, assuming 4 bytes/pixel", format);
            pixel_bytes = 4;
            break;
    }

    if (!createStagingBuffer((VkDeviceSize)w * h * pixel_bytes)) {
        return false;
    }

    MLOG_INFO("VkImg", "Created %dx%d fmt=%d (%u bytes/px)", w, h, format, pixel_bytes);
    return true;
}

bool VulkanImage::createStagingBuffer(VkDeviceSize size) {
    VkDevice dev = ctx_->device();
    staging_size_ = size;

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    if (vkCreateBuffer(dev, &bi, nullptr, &staging_) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "createBuffer failed");
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, staging_, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx_->findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        MLOG_ERROR("VkImg", "No host-visible memory");
        return false;
    }

    if (vkAllocateMemory(dev, &ai, nullptr, &staging_mem_) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "staging alloc failed");
        return false;
    }
    if (vkBindBufferMemory(dev, staging_, staging_mem_, 0) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "vkBindBufferMemory failed");
        return false;
    }

    // Persistently map staging buffer (Optimization D)
    if (vkMapMemory(dev, staging_mem_, 0, staging_size_, 0, &staging_mapped_) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "persistent map failed");
        staging_mapped_ = nullptr;
        return false;
    }

    return true;
}

bool VulkanImage::upload(VkCommandPool cmd_pool, VkQueue queue,
                          const void* data, size_t size) {
    if (!ctx_ || !image_) return false;
    if (size > staging_size_) {
        MLOG_ERROR("VkImg", "Upload size %zu exceeds staging %llu", size, staging_size_);
        return false;
    }

    // Copy to persistently mapped staging (no map/unmap needed)
    memcpy(staging_mapped_, data, size);

    return uploadFromStaging(cmd_pool, queue);
}

bool VulkanImage::uploadFromStaging(VkCommandPool cmd_pool, VkQueue queue) {
    if (!ctx_ || !image_) return false;
    VkDevice dev = ctx_->device();

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = cmd_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "vkAllocateCommandBuffers failed");
        return false;
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    transitionLayout(cmd,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyBufferToImage(cmd, staging_, image_,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "vkQueueSubmit failed");
        vkFreeCommandBuffers(dev, cmd_pool, 1, &cmd);
        return false;
    }
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(dev, cmd_pool, 1, &cmd);
    return true;
}

bool VulkanImage::download(VkCommandPool cmd_pool, VkQueue queue,
                            void* out_data, size_t size) {
    if (!ctx_ || !image_) return false;
    if (size > staging_size_) {
        MLOG_ERROR("VkImg", "Download size %zu exceeds staging %llu", size, staging_size_);
        return false;
    }

    VkDevice dev = ctx_->device();

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = cmd_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "vkAllocateCommandBuffers failed");
        return false;
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    transitionLayout(cmd,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            staging_, 1, &region);

    transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        MLOG_ERROR("VkImg", "vkQueueSubmit failed");
        vkFreeCommandBuffers(dev, cmd_pool, 1, &cmd);
        return false;
    }
    vkQueueWaitIdle(queue);

    // Read from persistently mapped staging (no map/unmap)
    memcpy(out_data, staging_mapped_, size);

    vkFreeCommandBuffers(dev, cmd_pool, 1, &cmd);
    return true;
}

void VulkanImage::transitionLayout(VkCommandBuffer cmd,
                                    VkImageLayout old_layout, VkImageLayout new_layout,
                                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    switch (old_layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            barrier.srcAccessMask = 0; break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; break;
        case VK_IMAGE_LAYOUT_GENERAL:
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT; break;
        default:
            barrier.srcAccessMask = 0; break;
    }

    switch (new_layout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; break;
        case VK_IMAGE_LAYOUT_GENERAL:
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT; break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; break;
        default:
            barrier.dstAccessMask = 0; break;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage,
                          0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanImage::destroy() {
    if (!ctx_) return;
    VkDevice dev = ctx_->device();
    vkDeviceWaitIdle(dev);

    if (view_)        { vkDestroyImageView(dev, view_, nullptr);   view_ = VK_NULL_HANDLE; }
    if (image_)       { vkDestroyImage(dev, image_, nullptr);      image_ = VK_NULL_HANDLE; }
    if (memory_)      { vkFreeMemory(dev, memory_, nullptr);       memory_ = VK_NULL_HANDLE; }
    // Unmap before destroying staging
    if (staging_mapped_ && staging_mem_) {
        vkUnmapMemory(dev, staging_mem_);
        staging_mapped_ = nullptr;
    }
    if (staging_)     { vkDestroyBuffer(dev, staging_, nullptr);   staging_ = VK_NULL_HANDLE; }
    if (staging_mem_) { vkFreeMemory(dev, staging_mem_, nullptr);  staging_mem_ = VK_NULL_HANDLE; }
    ctx_ = nullptr;
}

} // namespace mirage::vk
