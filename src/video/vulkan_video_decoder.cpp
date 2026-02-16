// =============================================================================
// MirageSystem - Vulkan Video H.264 Decoder Implementation
// =============================================================================
// Main file: Initialization, configuration, decode entry points
//
// Split into multiple files for maintainability:
//   - vulkan_video_session.cpp: Video session management
//   - vulkan_video_dpb.cpp: DPB (Decoded Picture Buffer) management
//   - vulkan_video_decode_impl.cpp: Slice decoding, POC calculation, MMCO
// =============================================================================

#include "vulkan_video_decoder.hpp"
#include "h264_parser.hpp"
#include "../mirage_log.hpp"

#include <cstring>
#include <algorithm>

namespace mirage::video {

// =============================================================================
// Helper: Load Vulkan Video extension functions (instance method)
// =============================================================================
static bool loadVulkanVideoFunctions(VkDevice device, VulkanVideoFunctions& vkfn) {
    #define LOAD_DEVICE_FUNC(name) \
        vkfn.name = (PFN_vk##name##KHR)vkGetDeviceProcAddr(device, "vk" #name "KHR"); \
        if (!vkfn.name) { MLOG_ERROR("VkVideo", "Failed to load " #name "KHR"); return false; }

    LOAD_DEVICE_FUNC(CreateVideoSession);
    LOAD_DEVICE_FUNC(DestroyVideoSession);
    LOAD_DEVICE_FUNC(GetVideoSessionMemoryRequirements);
    LOAD_DEVICE_FUNC(BindVideoSessionMemory);
    LOAD_DEVICE_FUNC(CreateVideoSessionParameters);
    LOAD_DEVICE_FUNC(UpdateVideoSessionParameters);
    LOAD_DEVICE_FUNC(DestroyVideoSessionParameters);
    LOAD_DEVICE_FUNC(CmdBeginVideoCoding);
    LOAD_DEVICE_FUNC(CmdEndVideoCoding);
    LOAD_DEVICE_FUNC(CmdControlVideoCoding);
    LOAD_DEVICE_FUNC(CmdDecodeVideo);

    #undef LOAD_DEVICE_FUNC

    return true;
}

// =============================================================================
// VulkanVideoDecoder Implementation
// =============================================================================

VulkanVideoDecoder::VulkanVideoDecoder() = default;

VulkanVideoDecoder::~VulkanVideoDecoder() {
    destroy();
}

bool VulkanVideoDecoder::isSupported(VkPhysicalDevice physical_device) {
    // Check for VK_KHR_video_decode_queue extension
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, nullptr);

    std::vector<VkExtensionProperties> extensions(ext_count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, extensions.data());

    bool has_video_queue = false;
    bool has_h264_decode = false;

    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) == 0) {
            has_video_queue = true;
        }
        if (strcmp(ext.extensionName, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME) == 0) {
            has_h264_decode = true;
        }
    }

    if (!has_video_queue || !has_h264_decode) {
        MLOG_INFO("VkVideo", "Vulkan Video H.264 decode not supported: video_queue=%d, h264=%d",
                  has_video_queue, has_h264_decode);
        return false;
    }

    MLOG_INFO("VkVideo", "Vulkan Video H.264 decode extensions available");
    return true;
}

bool VulkanVideoDecoder::queryCapabilities(VkInstance instance,
                                            VkPhysicalDevice physical_device,
                                            VulkanVideoCapabilities& caps) {
    caps = {};  // Reset

    if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE) {
        MLOG_ERROR("VkVideo", "queryCapabilities: VkInstance or VkPhysicalDevice is null");
        return false;
    }

    // Check extension support first
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> extensions(ext_count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, extensions.data());

    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME) == 0) {
            caps.supports_h264_decode = true;
        }
        if (strcmp(ext.extensionName, "VK_KHR_video_decode_h265") == 0) {
            caps.supports_h265_decode = true;
        }
    }

    if (!caps.supports_h264_decode) {
        MLOG_WARN("VkVideo", "H.264 decode extension not supported");
        return false;
    }

    // Build H.264 decode profile for capabilities query
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

    // Build pNext chain for capabilities query
    VkVideoDecodeH264CapabilitiesKHR h264_caps = {};
    h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;

    VkVideoDecodeCapabilitiesKHR decode_caps = {};
    decode_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    decode_caps.pNext = &h264_caps;

    VkVideoCapabilitiesKHR video_caps = {};
    video_caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    video_caps.pNext = &decode_caps;

    // Get the function pointer using the provided VkInstance
    auto pfnGetPhysicalDeviceVideoCapabilitiesKHR =
        (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR");

    if (pfnGetPhysicalDeviceVideoCapabilitiesKHR) {
        VkResult result = pfnGetPhysicalDeviceVideoCapabilitiesKHR(
            physical_device, &profile_info, &video_caps);

        if (result == VK_SUCCESS) {
            // Extract capabilities from query result
            caps.max_width = video_caps.maxCodedExtent.width;
            caps.max_height = video_caps.maxCodedExtent.height;
            caps.min_width = video_caps.minCodedExtent.width;
            caps.min_height = video_caps.minCodedExtent.height;
            caps.max_dpb_slots = video_caps.maxDpbSlots;
            caps.max_active_reference_pictures = video_caps.maxActiveReferencePictures;
            caps.minBitstreamBufferOffsetAlignment = video_caps.minBitstreamBufferOffsetAlignment;
            caps.minBitstreamBufferSizeAlignment = video_caps.minBitstreamBufferSizeAlignment;

            // Extract H.264 specific capabilities
            caps.max_level_idc = static_cast<uint8_t>(h264_caps.maxLevelIdc);

            // Store header version for pStdHeaderVersion
            caps.stdHeaderVersion = video_caps.stdHeaderVersion;
            caps.hasStdHeaderVersion = true;

            MLOG_INFO("VkVideo", "Queried video capabilities: max=%ux%u, min=%ux%u, DPB=%u, refs=%u, level=%u",
                      caps.max_width, caps.max_height,
                      caps.min_width, caps.min_height,
                      caps.max_dpb_slots, caps.max_active_reference_pictures, caps.max_level_idc);
            MLOG_INFO("VkVideo", "Bitstream alignment: offset=%llu, size=%llu",
                      (unsigned long long)caps.minBitstreamBufferOffsetAlignment,
                      (unsigned long long)caps.minBitstreamBufferSizeAlignment);
            MLOG_INFO("VkVideo", "Std header version: %s (spec %u.%u.%u)",
                      caps.stdHeaderVersion.extensionName,
                      VK_API_VERSION_MAJOR(caps.stdHeaderVersion.specVersion),
                      VK_API_VERSION_MINOR(caps.stdHeaderVersion.specVersion),
                      VK_API_VERSION_PATCH(caps.stdHeaderVersion.specVersion));

            return true;
        } else {
            MLOG_WARN("VkVideo", "vkGetPhysicalDeviceVideoCapabilitiesKHR failed: %d, using defaults", result);
        }
    } else {
        MLOG_WARN("VkVideo", "vkGetPhysicalDeviceVideoCapabilitiesKHR not available, using defaults");
    }

    // Fallback: Set reasonable defaults based on common GPU capabilities
    caps.max_width = 4096;
    caps.max_height = 4096;
    caps.min_width = 16;
    caps.min_height = 16;
    caps.max_dpb_slots = 17;  // H.264 Level 5.1 max
    caps.max_active_reference_pictures = 16;
    caps.max_level_idc = 51;  // Level 5.1
    caps.minBitstreamBufferOffsetAlignment = 256;  // Conservative default
    caps.minBitstreamBufferSizeAlignment = 256;
    caps.hasStdHeaderVersion = false;

    // Adjust based on device limits
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device, &props);

    if (props.limits.maxImageDimension2D < caps.max_width) {
        caps.max_width = props.limits.maxImageDimension2D;
        caps.max_height = props.limits.maxImageDimension2D;
    }

    MLOG_INFO("VkVideo", "Using default video capabilities: max=%ux%u, DPB=%u, refs=%u, level=%u",
              caps.max_width, caps.max_height,
              caps.max_dpb_slots, caps.max_active_reference_pictures, caps.max_level_idc);

    return caps.supports_h264_decode;
}

bool VulkanVideoDecoder::initialize(VkInstance instance,
                                    VkPhysicalDevice physical_device,
                                    VkDevice device,
                                    uint32_t video_decode_queue_family,
                                    VkQueue video_decode_queue,
                                    const VulkanVideoDecoderConfig& config) {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (initialized_) {
        MLOG_WARN("VkVideo", "Decoder already initialized");
        return true;
    }

    if (instance == VK_NULL_HANDLE) {
        MLOG_ERROR("VkVideo", "VkInstance is required for initialization");
        return false;
    }

    instance_ = instance;
    physical_device_ = physical_device;
    device_ = device;
    video_queue_ = video_decode_queue;
    video_queue_family_ = video_decode_queue_family;
    config_ = config;

    // Query video decode capabilities
    if (!queryCapabilities(instance, physical_device, capabilities_)) {
        MLOG_ERROR("VkVideo", "Failed to query video capabilities");
        return false;
    }

    // Load extension functions (per-instance)
    if (!loadVulkanVideoFunctions(device, vkfn_)) {
        MLOG_ERROR("VkVideo", "Failed to load Vulkan Video functions");
        return false;
    }

    // Create command pool for video decode queue
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = video_queue_family_;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(device_, &pool_info, nullptr, &cmd_pool_) != VK_SUCCESS) {
        MLOG_ERROR("VkVideo", "Failed to create command pool");
        return false;
    }

    // Allocate command buffers for frame-in-flight
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = cmd_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> cmd_buffers;
    if (vkAllocateCommandBuffers(device_, &alloc_info, cmd_buffers.data()) != VK_SUCCESS) {
        MLOG_ERROR("VkVideo", "Failed to allocate command buffers");
        destroy();
        return false;
    }

    // Initialize frame resources
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        frame_resources_[i].cmd_buffer = cmd_buffers[i];
        frame_resources_[i].timeline_value = 0;
        frame_resources_[i].in_use = false;
    }

    // Create initial bitstream buffers for each frame (512KB each)
    const size_t initial_buffer_size = 512 * 1024;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!createFrameBitstreamBuffer(i, initial_buffer_size)) {
            MLOG_ERROR("VkVideo", "Failed to create bitstream buffer for frame %u", i);
            destroy();
            return false;
        }
    }

    MLOG_INFO("VkVideo", "Created %u frame-in-flight resources", MAX_FRAMES_IN_FLIGHT);

    // Create timeline semaphore for async decode
    VkSemaphoreTypeCreateInfo timeline_info = {};
    timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_info.initialValue = 0;

    VkSemaphoreCreateInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_info.pNext = &timeline_info;

    if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &timeline_semaphore_) != VK_SUCCESS) {
        MLOG_ERROR("VkVideo", "Failed to create timeline semaphore");
        destroy();
        return false;
    }
    timeline_value_ = 0;

    // Reserve space for SPS/PPS
    sps_list_.resize(32);
    pps_list_.resize(256);

    initialized_ = true;
    MLOG_INFO("VkVideo", "Vulkan Video decoder initialized (max %dx%d, %d DPB slots)",
              config_.max_width, config_.max_height, config_.dpb_slot_count);

    return true;
}

void VulkanVideoDecoder::destroy() {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (!initialized_) return;

    vkDeviceWaitIdle(device_);

    destroyVideoSession();
    destroyFrameBitstreamBuffers();
    freeDpbSlots();

    if (cmd_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        cmd_pool_ = VK_NULL_HANDLE;
    }

    if (timeline_semaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
        timeline_semaphore_ = VK_NULL_HANDLE;
    }

    // Reset frame resources
    for (auto& fr : frame_resources_) {
        fr.cmd_buffer = VK_NULL_HANDLE;
        fr.timeline_value = 0;
        fr.in_use = false;
    }
    current_frame_index_ = 0;

    sps_list_.clear();
    pps_list_.clear();

    initialized_ = false;
    MLOG_INFO("VkVideo", "Vulkan Video decoder destroyed");
}

// =============================================================================
// Decode Entry Points
// =============================================================================

DecodeResult VulkanVideoDecoder::decode(const uint8_t* nal_data, size_t nal_size, int64_t pts) {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    DecodeResult result;
    result.pts = pts;

    if (!initialized_ || !nal_data || nal_size == 0) {
        result.error_message = "Invalid input or not initialized";
        errors_count_++;
        return result;
    }

    // Parse NAL unit
    H264Parser parser;
    auto nals = parser.parseAnnexB(nal_data, nal_size);

    for (const auto& nal : nals) {
        switch (static_cast<NalUnitType>(nal.nal_unit_type)) {
            case NalUnitType::SPS: {
                auto sps = std::make_unique<H264SPS>();
                if (parser.parseSPS(nal.rbsp_data, nal.rbsp_size, *sps)) {
                    uint8_t id = sps->sps_id;
                    if (id < sps_list_.size()) {
                        sps_list_[id] = std::move(sps);
                        MLOG_INFO("VkVideo", "Parsed SPS %d: %dx%d",
                                  id, sps_list_[id]->width(), sps_list_[id]->height());

                        // Update resolution
                        current_width_ = sps_list_[id]->width();
                        current_height_ = sps_list_[id]->height();

                        // Create/recreate video session if needed
                        if (video_session_ == VK_NULL_HANDLE) {
                            if (!createVideoSession()) {
                                result.error_message = "Failed to create video session";
                                errors_count_++;
                                return result;
                            }
                        }
                    }
                }
                break;
            }

            case NalUnitType::PPS: {
                auto pps = std::make_unique<H264PPS>();
                if (parser.parsePPS(nal.rbsp_data, nal.rbsp_size, *pps)) {
                    uint8_t id = pps->pps_id;
                    if (id < pps_list_.size()) {
                        pps_list_[id] = std::move(pps);
                        MLOG_INFO("VkVideo", "Parsed PPS %d (SPS ref: %d)",
                                  id, pps_list_[id]->sps_id);

                        // Update session parameters
                        createVideoSessionParameters();
                    }
                }
                break;
            }

            case NalUnitType::SLICE_NON_IDR:
            case NalUnitType::SLICE_IDR: {
                // Pass original NAL data with start code for Vulkan Video bitstream
                // decodeSlice will skip start code internally when parsing
                result = decodeSlice(nal.data, nal.size, pts);
                if (result.success && frame_callback_) {
                    frame_callback_(result.output_image, result.output_view,
                                   result.width, result.height, pts);
                }
                break;
            }

            default:
                // Skip other NAL types (SEI, AUD, etc.)
                break;
        }
    }

    return result;
}

std::vector<DecodeResult> VulkanVideoDecoder::decodeAccessUnit(const uint8_t* data, size_t size, int64_t pts) {
    std::vector<DecodeResult> results;
    auto result = decode(data, size, pts);
    if (result.success) {
        results.push_back(result);
    }
    return results;
}

std::vector<DecodeResult> VulkanVideoDecoder::flush() {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    std::vector<DecodeResult> results;

    if (!initialized_ || dpb_slots_.empty()) {
        return results;
    }

    // Collect all active DPB slots with their POC
    struct DpbFrame {
        int32_t slot_index;
        int32_t poc;
    };
    std::vector<DpbFrame> active_frames;

    for (size_t i = 0; i < dpb_slots_.size(); i++) {
        if (dpb_slots_[i].in_use && dpb_slots_[i].is_reference) {
            active_frames.push_back({static_cast<int32_t>(i), dpb_slots_[i].poc});
        }
    }

    // Sort by POC for display order output
    std::sort(active_frames.begin(), active_frames.end(),
              [](const DpbFrame& a, const DpbFrame& b) { return a.poc < b.poc; });

    // Output frames in POC order
    for (const auto& frame : active_frames) {
        auto& slot = dpb_slots_[frame.slot_index];

        DecodeResult result;
        result.success = true;
        result.output_image = slot.image;
        result.output_view = slot.view;
        result.width = current_width_;
        result.height = current_height_;
        result.poc = slot.poc;
        result.pts = 0;  // PTS unknown for flushed frames

        results.push_back(result);

        // Call frame callback if set
        if (frame_callback_) {
            frame_callback_(slot.image, slot.view, current_width_, current_height_, 0);
        }

        // Mark slot as no longer reference (consumed)
        slot.is_reference = false;
        slot.in_use = false;
    }

    // Reset POC state for next stream
    prev_poc_msb_ = 0;
    prev_poc_lsb_ = 0;
    frame_num_offset_ = 0;
    prev_frame_num_offset_ = 0;
    prev_frame_num_ = 0;
    prev_poc_ = 0;
    first_slice_ = true;

    // Flush reorder buffer
    outputReorderedFrames(true);
    reorder_buffer_.clear();
    last_output_poc_ = INT32_MIN;

    MLOG_INFO("VkVideo", "Flushed %zu frames from DPB", results.size());

    return results;
}

} // namespace mirage::video
