// =============================================================================
// MirageSystem - Vulkan Video DPB (Decoded Picture Buffer) Management
// =============================================================================
// Split from vulkan_video_decoder.cpp for maintainability
// Contains: DPB allocation, bitstream buffer management, frame resources
// =============================================================================

#include "vulkan_video_decoder.hpp"
#include "../mirage_log.hpp"

#include <cstring>

namespace mirage::video {

bool VulkanVideoDecoder::allocateDpbSlots() {
    freeDpbSlots();

    dpb_slots_.resize(config_.dpb_slot_count);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);

    for (uint32_t i = 0; i < config_.dpb_slot_count; i++) {
        auto& slot = dpb_slots_[i];

        // Create NV12 image for DPB
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        image_info.extent.width = config_.max_width;
        image_info.extent.height = config_.max_height;
        image_info.extent.depth = 1;
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                          VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // For YUV conversion/copy
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // Add video profile
        VkVideoDecodeH264ProfileInfoKHR h264_profile = {};
        h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
        h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
        h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;

        VkVideoProfileInfoKHR profile_info = {};
        profile_info.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
        profile_info.pNext = &h264_profile;
        profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        profile_info.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        profile_info.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        profile_info.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

        VkVideoProfileListInfoKHR profile_list = {};
        profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
        profile_list.profileCount = 1;
        profile_list.pProfiles = &profile_info;

        image_info.pNext = &profile_list;

        if (vkCreateImage(device_, &image_info, nullptr, &slot.image) != VK_SUCCESS) {
            MLOG_ERROR("VkVideo", "Failed to create DPB image %d", i);
            freeDpbSlots();
            return false;
        }

        // Allocate memory
        VkMemoryRequirements mem_req;
        vkGetImageMemoryRequirements(device_, slot.image, &mem_req);

        uint32_t mem_type_index = UINT32_MAX;
        for (uint32_t j = 0; j < mem_props.memoryTypeCount; j++) {
            if ((mem_req.memoryTypeBits & (1 << j)) &&
                (mem_props.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                mem_type_index = j;
                break;
            }
        }

        if (mem_type_index == UINT32_MAX) {
            MLOG_ERROR("VkVideo", "No suitable memory type for DPB");
            freeDpbSlots();
            return false;
        }

        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_req.size;
        alloc_info.memoryTypeIndex = mem_type_index;

        if (vkAllocateMemory(device_, &alloc_info, nullptr, &slot.memory) != VK_SUCCESS) {
            MLOG_ERROR("VkVideo", "Failed to allocate DPB memory %d", i);
            freeDpbSlots();
            return false;
        }

        if (vkBindImageMemory(device_, slot.image, slot.memory, 0) != VK_SUCCESS) {
            MLOG_ERROR("VkVideo", "Failed to bind DPB memory %d", i);
            freeDpbSlots();
            return false;
        }

        // Create image view for video decode output
        // For Vulkan Video decode DPB, use VK_IMAGE_ASPECT_COLOR_BIT
        // This is the correct aspect for video decode reference pictures
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = slot.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &view_info, nullptr, &slot.view) != VK_SUCCESS) {
            MLOG_ERROR("VkVideo", "Failed to create DPB view %d", i);
            freeDpbSlots();
            return false;
        }

        slot.reset();
    }

    MLOG_INFO("VkVideo", "Allocated %d DPB slots (%dx%d NV12)",
              config_.dpb_slot_count, config_.max_width, config_.max_height);

    return true;
}

void VulkanVideoDecoder::freeDpbSlots() {
    for (auto& slot : dpb_slots_) {
        if (slot.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, slot.view, nullptr);
            slot.view = VK_NULL_HANDLE;
        }
        if (slot.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, slot.image, nullptr);
            slot.image = VK_NULL_HANDLE;
        }
        if (slot.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, slot.memory, nullptr);
            slot.memory = VK_NULL_HANDLE;
        }
    }
    dpb_slots_.clear();
}

int32_t VulkanVideoDecoder::acquireDpbSlot() {
    for (size_t i = 0; i < dpb_slots_.size(); i++) {
        if (!dpb_slots_[i].in_use) {
            dpb_slots_[i].in_use = true;
            return static_cast<int32_t>(i);
        }
    }

    // Find LRU non-reference slot
    for (size_t i = 0; i < dpb_slots_.size(); i++) {
        if (!dpb_slots_[i].is_reference) {
            dpb_slots_[i].reset();
            dpb_slots_[i].in_use = true;
            return static_cast<int32_t>(i);
        }
    }

    // Force reuse slot with minimum POC (oldest picture)
    int oldest_slot = 0;
    int32_t min_poc = INT32_MAX;
    for (size_t i = 0; i < dpb_slots_.size(); i++) {
        if (dpb_slots_[i].poc < min_poc) {
            min_poc = dpb_slots_[i].poc;
            oldest_slot = static_cast<int>(i);
        }
    }
    dpb_slots_[oldest_slot].reset();
    dpb_slots_[oldest_slot].in_use = true;
    return oldest_slot;
}

void VulkanVideoDecoder::releaseDpbSlot(int32_t index) {
    if (index >= 0 && index < static_cast<int32_t>(dpb_slots_.size())) {
        dpb_slots_[index].in_use = false;
    }
}

int32_t VulkanVideoDecoder::findDpbSlotByPoc(int32_t poc) {
    for (size_t i = 0; i < dpb_slots_.size(); i++) {
        if (dpb_slots_[i].in_use && dpb_slots_[i].poc == poc) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool VulkanVideoDecoder::createFrameBitstreamBuffer(uint32_t frame_index, size_t size) {
    if (frame_index >= MAX_FRAMES_IN_FLIGHT) return false;

    auto& fr = frame_resources_[frame_index];

    // Destroy existing buffer if any
    if (fr.bitstream_mapped) {
        vkUnmapMemory(device_, fr.bitstream_memory);
        fr.bitstream_mapped = nullptr;
    }
    if (fr.bitstream_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, fr.bitstream_memory, nullptr);
        fr.bitstream_memory = VK_NULL_HANDLE;
    }
    if (fr.bitstream_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, fr.bitstream_buffer, nullptr);
        fr.bitstream_buffer = VK_NULL_HANDLE;
    }

    // Align buffer size
    VkDeviceSize alignment = capabilities_.minBitstreamBufferSizeAlignment;
    if (alignment == 0) alignment = 1;
    VkDeviceSize aligned_size = ((size + alignment - 1) / alignment) * alignment;

    // Create buffer with video profile
    VkVideoDecodeH264ProfileInfoKHR h264_profile = {};
    h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;

    VkVideoProfileInfoKHR profile_info = {};
    profile_info.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profile_info.pNext = &h264_profile;
    profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    profile_info.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile_info.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profile_info.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    VkVideoProfileListInfoKHR profile_list = {};
    profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profile_list.profileCount = 1;
    profile_list.pProfiles = &profile_info;

    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.pNext = &profile_list;
    buffer_info.size = aligned_size;
    buffer_info.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &buffer_info, nullptr, &fr.bitstream_buffer) != VK_SUCCESS) {
        MLOG_ERROR("VkVideo", "Failed to create frame %u bitstream buffer", frame_index);
        return false;
    }

    // Allocate host-visible memory
    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device_, fr.bitstream_buffer, &mem_req);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);

    uint32_t mem_type_index = UINT32_MAX;
    VkMemoryPropertyFlags desired = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & desired) == desired) {
            mem_type_index = i;
            break;
        }
    }

    if (mem_type_index == UINT32_MAX) {
        vkDestroyBuffer(device_, fr.bitstream_buffer, nullptr);
        fr.bitstream_buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type_index;

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &fr.bitstream_memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, fr.bitstream_buffer, nullptr);
        fr.bitstream_buffer = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindBufferMemory(device_, fr.bitstream_buffer, fr.bitstream_memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, fr.bitstream_memory, nullptr);
        vkDestroyBuffer(device_, fr.bitstream_buffer, nullptr);
        fr.bitstream_buffer = VK_NULL_HANDLE;
        fr.bitstream_memory = VK_NULL_HANDLE;
        return false;
    }

    if (vkMapMemory(device_, fr.bitstream_memory, 0, aligned_size, 0, &fr.bitstream_mapped) != VK_SUCCESS) {
        vkFreeMemory(device_, fr.bitstream_memory, nullptr);
        vkDestroyBuffer(device_, fr.bitstream_buffer, nullptr);
        fr.bitstream_buffer = VK_NULL_HANDLE;
        fr.bitstream_memory = VK_NULL_HANDLE;
        return false;
    }

    fr.bitstream_buffer_size = aligned_size;
    return true;
}

void VulkanVideoDecoder::destroyFrameBitstreamBuffers() {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        auto& fr = frame_resources_[i];
        if (fr.bitstream_mapped) {
            vkUnmapMemory(device_, fr.bitstream_memory);
            fr.bitstream_mapped = nullptr;
        }
        if (fr.bitstream_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, fr.bitstream_memory, nullptr);
            fr.bitstream_memory = VK_NULL_HANDLE;
        }
        if (fr.bitstream_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, fr.bitstream_buffer, nullptr);
            fr.bitstream_buffer = VK_NULL_HANDLE;
        }
        fr.bitstream_buffer_size = 0;
        fr.in_use = false;
    }
}

uint32_t VulkanVideoDecoder::acquireFrameResources() {
    // Find a frame slot that is not in use, or wait for the oldest one
    uint32_t frame_index = current_frame_index_;
    auto& fr = frame_resources_[frame_index];

    // If this frame is still in flight, wait for it
    if (fr.in_use && fr.timeline_value > 0) {
        VkSemaphoreWaitInfo wait_info = {};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &timeline_semaphore_;
        wait_info.pValues = &fr.timeline_value;

        // Wait with timeout (100ms)
        VkResult result = vkWaitSemaphores(device_, &wait_info, 100000000ULL);
        if (result == VK_TIMEOUT) {
            MLOG_WARN("VkVideo", "Frame %u decode timeout, waiting longer", frame_index);
            vkWaitSemaphores(device_, &wait_info, UINT64_MAX);
        }
    }

    fr.in_use = true;
    current_frame_index_ = (current_frame_index_ + 1) % MAX_FRAMES_IN_FLIGHT;
    return frame_index;
}

void VulkanVideoDecoder::releaseFrameResources(uint32_t frame_index) {
    if (frame_index < MAX_FRAMES_IN_FLIGHT) {
        frame_resources_[frame_index].in_use = false;
    }
}

} // namespace mirage::video
