// =============================================================================
// MirageSystem - Vulkan Texture (for ImGui display)
// =============================================================================
#include "vulkan_texture.hpp"
#include "mirage_log.hpp"
#include <imgui_impl_vulkan.h>
#include <cstring>
#include <chrono>


// === Debug: lightweight RGBA hash to detect frozen input ===
static uint64_t QuickRgbaHash(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return 0;
    const int sx = 6;
    const int sy = 6;
    uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < sy; y++) {
        int py = (h - 1) * y / (sy - 1);
        for (int x = 0; x < sx; x++) {
            int px = (w - 1) * x / (sx - 1);
            const uint8_t* p = rgba + (py * w + px) * 4;
            for (int k = 0; k < 4; k++) {
                hash ^= (uint64_t)p[k];
                hash *= 1099511628211ull;
            }
        }
    }
    return hash;
}

namespace mirage::vk {

static inline uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool VulkanTexture::create(VulkanContext& ctx, VkDescriptorPool /*pool*/, int w, int h) {
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
    if (vkBindBufferMemory(dev, staging_, staging_mem_, 0) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "vkBindBufferMemory failed"); return false;
    }

    // Persistent map (HOST_COHERENT)
    if (vkMapMemory(dev, staging_mem_, 0, staging_size_, 0, &staging_mapped_) != VK_SUCCESS || !staging_mapped_) {
        MLOG_ERROR("VkTex", "vkMapMemory staging failed");
        return false;
    }

    // Fence for async uploads
    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first update won't block
    if (vkCreateFence(dev, &fci, nullptr, &upload_fence_) != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "vkCreateFence failed");
        return false;
    }

    layout_initialized_ = false;
    last_submit_ms_ = 0;
    skipped_updates_ = 0;
    MLOG_INFO("VkTex", "Created %dx%d", w, h);
    return true;
}


void VulkanTexture::clear(VkCommandPool cmd_pool, VkQueue queue, uint32_t rgba) {
    if (!ctx_ || !image_) return;
    VkDevice dev = ctx_->device();

    // Wait previous upload (short) to avoid fighting fences
    if (upload_fence_) {
        vkWaitForFences(dev, 1, &upload_fence_, VK_TRUE, 50'000'000ULL); // 50ms max
        vkResetFences(dev, 1, &upload_fence_);
    }

    VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmd_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb{};
    if (vkAllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS) return;

    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier b1{}; b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = image_;
    b1.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b1.srcAccessMask = 0;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b1);

    // Clear
    VkClearColorValue cv{};
    cv.float32[0] = 0.0f;
    cv.float32[1] = 0.0f;
    cv.float32[2] = 0.0f;
    cv.float32[3] = 1.0f;
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);

    // Transition to SHADER_READ
    VkImageMemoryBarrier b2{}; b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = image_;
    b2.subresourceRange = range;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b2);

    vkEndCommandBuffer(cb);

    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(queue, 1, &si, upload_fence_);
    // Wait briefly for clear to complete so the first frame never shows stale VRAM
    vkWaitForFences(dev, 1, &upload_fence_, VK_TRUE, 200'000'000ULL);

    vkFreeCommandBuffers(dev, cmd_pool, 1, &cb);
    layout_initialized_ = true;
}

void VulkanTexture::update(VkCommandPool cmd_pool, VkQueue queue,
                            const uint8_t* rgba, int w, int h)
{
    if (!ctx_ || !image_ || w != width_ || h != height_) {
        static uint32_t early_ret = 0;
        early_ret++;
        if (early_ret <= 5 || early_ret % 300 == 0) {
            MLOG_WARN("VkTex", "update early-return#%u: ctx=%p image=%p w=%d/tex=%d h=%d/tex=%d",
                      early_ret, (void*)ctx_, (void*)image_, w, width_, h, height_);
        }
        return;
    }
    VkDevice dev = ctx_->device();

    // If previous upload is still in-flight, wait up to 2ms then skip if still busy.
    if (upload_fence_ != VK_NULL_HANDLE) {
        VkResult st = vkGetFenceStatus(dev, upload_fence_);
        if (st == VK_NOT_READY) {
            st = vkWaitForFences(dev, 1, &upload_fence_, VK_TRUE, 2'000'000);
        }
        if (st == VK_NOT_READY) {
            skipped_updates_++;
            if (skipped_updates_ % 60 == 1) {
                MLOG_WARN("VkTex", "update SKIP fence still NOT_READY after 2ms wait: skip#%u w=%d h=%d",
                          skipped_updates_, w, h);
            }
            return;
        }
        if (skipped_updates_ > 0 && skipped_updates_ % 60 == 0) {
            MLOG_WARN("VkTex", "upload resumed after %u skips", skipped_updates_);
        }
        vkResetFences(dev, 1, &upload_fence_);
    }
    // Per-instance update counter with periodic log
    update_count_++;
    if (update_count_ <= 5 || update_count_ % 300 == 0) {
        MLOG_INFO("VkTex", "update#%u pool=%p cache_pool=%p w=%d h=%d skip=%u",
                  update_count_, (void*)cmd_pool, (void*)cached_cmd_pool_, w, h, skipped_updates_);

    // Debug: hash sampled pixels to confirm input actually changes
    static uint64_t last_hash = 0;
    static int hash_cnt = 0;
    hash_cnt++;
    if (hash_cnt < 20 || (hash_cnt % 300 == 0)) {
        uint64_t h64 = QuickRgbaHash(rgba, w, h);
        MLOG_INFO("VkTex", "InputHash update#%d w=%d h=%d hash=%llu same=%d",\
                  update_count_, w, h, (unsigned long long)h64, (h64 == last_hash));
        last_hash = h64;
    }

    }

    // Copy to staging (persistently mapped)
    memcpy(staging_mapped_, rgba, (size_t)w * h * 4);

    // Allocate command buffer once, then reuse via reset
    if (cached_cmd_ == VK_NULL_HANDLE || cached_cmd_pool_ != cmd_pool) {
        if (cached_cmd_ != VK_NULL_HANDLE && cached_cmd_pool_ != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(dev, cached_cmd_pool_, 1, &cached_cmd_);
        }
        VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &cai, &cached_cmd_) != VK_SUCCESS) {
            MLOG_ERROR("VkTex", "vkAllocateCommandBuffers failed");
            cached_cmd_ = VK_NULL_HANDLE;
            cached_cmd_pool_ = VK_NULL_HANDLE;
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
    last_submit_ms_ = now_ms();
    skipped_updates_ = 0;

    VkResult submit_result = vkQueueSubmit(queue, 1, &si, upload_fence_);
    if (submit_result != VK_SUCCESS) {
        MLOG_ERROR("VkTex", "vkQueueSubmit FAILED result=%d update#%u", (int)submit_result, update_count_);
        vkQueueWaitIdle(queue);
        return;
    }
    if (update_count_ <= 5 || update_count_ % 300 == 0) {
        MLOG_INFO("VkTex", "vkQueueSubmit OK update#%u", update_count_);
    }

    layout_initialized_ = true;
}

// =============================================================================
// Integrated upload path (no separate vkQueueSubmit)
// =============================================================================

bool VulkanTexture::stageUpdate(const uint8_t* rgba, int w, int h) {
    if (!ctx_ || !image_ || w != width_ || h != height_ || !staging_mapped_) return false;
    memcpy(staging_mapped_, rgba, (size_t)w * h * 4);
    has_pending_upload_ = true;
    update_count_++;
    if (update_count_ <= 5 || update_count_ % 300 == 0) {
        MLOG_INFO("VkTex", "stageUpdate#%u w=%d h=%d", update_count_, w, h);
    }
    return true;
}

bool VulkanTexture::recordUpdate(VkCommandBuffer cmd) {
    if (!has_pending_upload_ || !ctx_ || !image_ || !staging_) return false;
    has_pending_upload_ = false;

    // Transition: SHADER_READ_ONLY (or UNDEFINED on first frame) -> TRANSFER_DST
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
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

    // Copy staging buffer -> image
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)width_, (uint32_t)height_, 1};
    vkCmdCopyBufferToImage(cmd, staging_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition: TRANSFER_DST -> SHADER_READ_ONLY
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    layout_initialized_ = true;
    return true;
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

    if (cached_cmd_ && cached_cmd_pool_) {
        vkFreeCommandBuffers(dev, cached_cmd_pool_, 1, &cached_cmd_);
        cached_cmd_ = VK_NULL_HANDLE;
        cached_cmd_pool_ = VK_NULL_HANDLE;
    }

    if (upload_fence_) {
        vkDestroyFence(dev, upload_fence_, nullptr);
        upload_fence_ = VK_NULL_HANDLE;
    }

    if (staging_mapped_) {
        vkUnmapMemory(dev, staging_mem_);
        staging_mapped_ = nullptr;
    }
    if (staging_) { vkDestroyBuffer(dev, staging_, nullptr); staging_ = VK_NULL_HANDLE; }
    if (staging_mem_) { vkFreeMemory(dev, staging_mem_, nullptr); staging_mem_ = VK_NULL_HANDLE; }

    ctx_ = nullptr;
}

} // namespace mirage::vk
