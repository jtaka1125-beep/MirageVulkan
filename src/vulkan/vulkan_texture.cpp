// =============================================================================
// MirageSystem - Vulkan Texture (for ImGui display)
// =============================================================================
#include "vulkan_texture.hpp"
#include "mirage_log.hpp"
#include <imgui_impl_vulkan.h>
#include <cstring>

namespace mirage::vk {

bool VulkanTexture::create(VulkanContext& ctx, VkDescriptorPool pool, int w, int h) {
    ctx_ = &ctx;
    width_ = w; height_ = h;
    VkDevice dev = ctx_->device();

    // Image
    VkImageCreateInfo ii{}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {(uint32_t)w, (uint32_t)h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(dev, &ii, nullptr, &image_) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "createImage failed"); return false;
    }

    VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, image_, &req);
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx_->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) { MLOG_ERROR("VkTex", "no device memory"); return false; }
    if (vkAllocateMemory(dev, &ai, nullptr, &memory_) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "allocMemory failed"); return false;
    }
    if (vkBindImageMemory(dev, image_, memory_, 0) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "vkBindImageMemory failed");
        return false;
    }

    // ImageView
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vi, nullptr, &view_) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "createImageView failed"); return false;
    }

    // Sampler
    VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &si, nullptr, &sampler_) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "createSampler failed"); return false;
    }

    // ImGui descriptor set
    imgui_ds_ = ImGui_ImplVulkan_AddTexture(sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!imgui_ds_) { MLOG_ERROR("VkTex", "ImGui_ImplVulkan_AddTexture failed"); return false; }

    // Staging buffer
    staging_size_ = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = staging_size_; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(dev, &bi, nullptr, &staging_) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "createBuffer failed"); return false;
    }
    VkMemoryRequirements breq; vkGetBufferMemoryRequirements(dev, staging_, &breq);
    VkMemoryAllocateInfo bai{}; bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bai.allocationSize = breq.size;
    bai.memoryTypeIndex = ctx_->findMemoryType(breq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev, &bai, nullptr, &staging_mem_) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "staging alloc failed"); return false;
    }
    vkBindBufferMemory(dev, staging_, staging_mem_, 0);

    layout_initialized_ = false;
    MLOG_INFO("VkTex", "Created %dx%d", w, h);
    return true;
}

void VulkanTexture::update(VkCommandPool cmd_pool, VkQueue queue,
                            const uint8_t* rgba, int w, int h)
{
    if (!ctx_ || !image_ || w != width_ || h != height_) return;
    VkDevice dev = ctx_->device();

    // Copy to staging
    void* mapped = nullptr;
    vkMapMemory(dev, staging_mem_, 0, staging_size_, 0, &mapped);
    memcpy(mapped, rgba, (size_t)w * h * 4);
    vkUnmapMemory(dev, staging_mem_);

    // Allocate command buffer once, then reuse via reset
    if (cached_cmd_ == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &cai, &cached_cmd_) != VK_SUCCESS) {
            MLOG_ERROR("VkTex", "vkAllocateCommandBuffers failed");
            return;
        }
        cached_cmd_pool_ = cmd_pool;
    }
    VkCommandBuffer cmd = cached_cmd_;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // Transition: UNDEFINED/SHADER_READ -> TRANSFER_DST
    VkImageMemoryBarrier bar{}; bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = layout_initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = image_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcAccessMask = layout_initialized_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        layout_initialized_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    // Copy buffer -> image
    VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cmd, staging_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition: TRANSFER_DST -> SHADER_READ
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "vkQueueSubmit failed");
        return;
    }
    vkQueueWaitIdle(queue);  // Simple sync for now

    layout_initialized_ = true;
}

void VulkanTexture::destroy() {
    if (!ctx_) return;
    VkDevice dev = ctx_->device();
    vkDeviceWaitIdle(dev);

    if (imgui_ds_) { ImGui_ImplVulkan_RemoveTexture(imgui_ds_); imgui_ds_ = VK_NULL_HANDLE; }
    if (sampler_) { vkDestroySampler(dev, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (view_) { vkDestroyImageView(dev, view_, nullptr); view_ = VK_NULL_HANDLE; }
    if (image_) { vkDestroyImage(dev, image_, nullptr); image_ = VK_NULL_HANDLE; }
    if (memory_) { vkFreeMemory(dev, memory_, nullptr); memory_ = VK_NULL_HANDLE; }
    if (staging_) { vkDestroyBuffer(dev, staging_, nullptr); staging_ = VK_NULL_HANDLE; }
    if (staging_mem_) { vkFreeMemory(dev, staging_mem_, nullptr); staging_mem_ = VK_NULL_HANDLE; }
    if (cached_cmd_ && cached_cmd_pool_) {
        vkFreeCommandBuffers(dev, cached_cmd_pool_, 1, &cached_cmd_);
        cached_cmd_ = VK_NULL_HANDLE;
        cached_cmd_pool_ = VK_NULL_HANDLE;
    }
    ctx_ = nullptr;
}

} // namespace mirage::vk
