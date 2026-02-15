// =============================================================================
// MirageSystem - Vulkan Video H.264 Decoder Implementation
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

bool VulkanVideoDecoder::createVideoSession() {
    if (video_session_ != VK_NULL_HANDLE) {
        destroyVideoSession();
    }

    // Video profile
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

    // Query capabilities for stdHeaderVersion
    VkVideoDecodeH264CapabilitiesKHR h264_caps = {};
    h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;

    VkVideoDecodeCapabilitiesKHR decode_caps = {};
    decode_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    decode_caps.pNext = &h264_caps;

    VkVideoCapabilitiesKHR caps = {};
    caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    caps.pNext = &decode_caps;

    // Try to query capabilities for stdHeaderVersion using stored instance
    auto pfnGetPhysicalDeviceVideoCapabilitiesKHR =
        (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)vkGetInstanceProcAddr(
            instance_, "vkGetPhysicalDeviceVideoCapabilitiesKHR");

    VkExtensionProperties stdHeaderVersion = {};
    const VkExtensionProperties* pStdHeaderVersion = nullptr;

    if (pfnGetPhysicalDeviceVideoCapabilitiesKHR) {
        VkResult caps_result = pfnGetPhysicalDeviceVideoCapabilitiesKHR(
            physical_device_, &profile_info, &caps);

        if (caps_result == VK_SUCCESS) {
            stdHeaderVersion = caps.stdHeaderVersion;
            pStdHeaderVersion = &stdHeaderVersion;
            MLOG_INFO("VkVideo", "Using stdHeaderVersion: %s (spec %u.%u.%u)",
                      stdHeaderVersion.extensionName,
                      VK_API_VERSION_MAJOR(stdHeaderVersion.specVersion),
                      VK_API_VERSION_MINOR(stdHeaderVersion.specVersion),
                      VK_API_VERSION_PATCH(stdHeaderVersion.specVersion));

            // Update capabilities with queried values
            if (caps.maxDpbSlots > 0) {
                capabilities_.max_dpb_slots = caps.maxDpbSlots;
            }
            if (caps.maxActiveReferencePictures > 0) {
                capabilities_.max_active_reference_pictures = caps.maxActiveReferencePictures;
            }
            capabilities_.minBitstreamBufferOffsetAlignment = caps.minBitstreamBufferOffsetAlignment;
            capabilities_.minBitstreamBufferSizeAlignment = caps.minBitstreamBufferSizeAlignment;
        } else {
            MLOG_WARN("VkVideo", "Failed to query capabilities for stdHeaderVersion: %d", caps_result);
        }
    }

    // Create video session
    VkVideoSessionCreateInfoKHR session_info = {};
    session_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    session_info.queueFamilyIndex = video_queue_family_;
    session_info.pVideoProfile = &profile_info;
    session_info.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;  // NV12
    session_info.maxCodedExtent.width = config_.max_width;
    session_info.maxCodedExtent.height = config_.max_height;
    session_info.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_info.maxDpbSlots = std::min(config_.dpb_slot_count, capabilities_.max_dpb_slots);
    session_info.maxActiveReferencePictures = std::min(config_.dpb_slot_count - 1, capabilities_.max_active_reference_pictures);
    session_info.pStdHeaderVersion = pStdHeaderVersion;  // Use queried version or nullptr

    VkResult result = vkfn_.CreateVideoSession(device_, &session_info, nullptr, &video_session_);
    if (result != VK_SUCCESS) {
        MLOG_ERROR("VkVideo", "Failed to create video session: %d", result);
        return false;
    }

    // Get and bind memory requirements
    uint32_t mem_req_count = 0;
    vkfn_.GetVideoSessionMemoryRequirements(device_, video_session_, &mem_req_count, nullptr);

    std::vector<VkVideoSessionMemoryRequirementsKHR> mem_reqs(mem_req_count);
    for (auto& req : mem_reqs) {
        req.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    }
    vkfn_.GetVideoSessionMemoryRequirements(device_, video_session_, &mem_req_count, mem_reqs.data());

    std::vector<VkBindVideoSessionMemoryInfoKHR> bind_infos;
    session_memory_.resize(mem_req_count);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);

    for (uint32_t i = 0; i < mem_req_count; i++) {
        const auto& req = mem_reqs[i].memoryRequirements;

        // Find suitable memory type
        uint32_t mem_type_index = UINT32_MAX;
        for (uint32_t j = 0; j < mem_props.memoryTypeCount; j++) {
            if ((req.memoryTypeBits & (1 << j)) &&
                (mem_props.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                mem_type_index = j;
                break;
            }
        }

        if (mem_type_index == UINT32_MAX) {
            MLOG_ERROR("VkVideo", "No suitable memory type for video session");
            destroyVideoSession();
            return false;
        }

        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = mem_type_index;

        if (vkAllocateMemory(device_, &alloc_info, nullptr, &session_memory_[i]) != VK_SUCCESS) {
            MLOG_ERROR("VkVideo", "Failed to allocate video session memory");
            destroyVideoSession();
            return false;
        }

        VkBindVideoSessionMemoryInfoKHR bind_info = {};
        bind_info.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        bind_info.memoryBindIndex = mem_reqs[i].memoryBindIndex;
        bind_info.memory = session_memory_[i];
        bind_info.memoryOffset = 0;
        bind_info.memorySize = req.size;
        bind_infos.push_back(bind_info);
    }

    result = vkfn_.BindVideoSessionMemory(device_, video_session_,
                                          static_cast<uint32_t>(bind_infos.size()),
                                          bind_infos.data());
    if (result != VK_SUCCESS) {
        MLOG_ERROR("VkVideo", "Failed to bind video session memory: %d", result);
        destroyVideoSession();
        return false;
    }

    // Allocate DPB slots
    if (!allocateDpbSlots()) {
        MLOG_ERROR("VkVideo", "Failed to allocate DPB slots");
        destroyVideoSession();
        return false;
    }

    MLOG_INFO("VkVideo", "Video session created: %dx%d, %d DPB slots",
              config_.max_width, config_.max_height, config_.dpb_slot_count);

    return true;
}

bool VulkanVideoDecoder::createVideoSessionParameters() {
    if (session_params_ != VK_NULL_HANDLE) {
        vkfn_.DestroyVideoSessionParameters(device_, session_params_, nullptr);
        session_params_ = VK_NULL_HANDLE;
    }

    // Collect active SPS/PPS
    std::vector<StdVideoH264SequenceParameterSet> std_sps_list;
    std::vector<StdVideoH264PictureParameterSet> std_pps_list;

    for (size_t i = 0; i < sps_list_.size(); i++) {
        if (sps_list_[i]) {
            const auto& sps = *sps_list_[i];

            StdVideoH264SequenceParameterSet std_sps = {};
            std_sps.flags.constraint_set0_flag = 0;
            std_sps.flags.constraint_set1_flag = 0;
            std_sps.flags.constraint_set2_flag = 0;
            std_sps.flags.constraint_set3_flag = 0;
            std_sps.flags.constraint_set4_flag = 0;
            std_sps.flags.constraint_set5_flag = 0;
            std_sps.flags.direct_8x8_inference_flag = sps.direct_8x8_inference_flag ? 1 : 0;
            std_sps.flags.mb_adaptive_frame_field_flag = 0;
            std_sps.flags.frame_mbs_only_flag = sps.frame_mbs_only_flag ? 1 : 0;
            std_sps.flags.delta_pic_order_always_zero_flag = sps.delta_pic_order_always_zero_flag ? 1 : 0;
            std_sps.flags.separate_colour_plane_flag = 0;
            std_sps.flags.gaps_in_frame_num_value_allowed_flag = sps.gaps_in_frame_num_allowed ? 1 : 0;
            std_sps.flags.qpprime_y_zero_transform_bypass_flag = 0;
            std_sps.flags.frame_cropping_flag = sps.frame_cropping_flag ? 1 : 0;
            std_sps.flags.seq_scaling_matrix_present_flag = 0;
            std_sps.flags.vui_parameters_present_flag = sps.vui_parameters_present ? 1 : 0;

            std_sps.profile_idc = static_cast<StdVideoH264ProfileIdc>(sps.profile_idc);
            std_sps.level_idc = static_cast<StdVideoH264LevelIdc>(sps.level_idc);
            std_sps.chroma_format_idc = static_cast<StdVideoH264ChromaFormatIdc>(sps.chroma_format_idc);
            std_sps.seq_parameter_set_id = sps.sps_id;
            std_sps.bit_depth_luma_minus8 = sps.bit_depth_luma - 8;
            std_sps.bit_depth_chroma_minus8 = sps.bit_depth_chroma - 8;
            std_sps.log2_max_frame_num_minus4 = sps.log2_max_frame_num - 4;
            std_sps.pic_order_cnt_type = static_cast<StdVideoH264PocType>(sps.pic_order_cnt_type);
            std_sps.log2_max_pic_order_cnt_lsb_minus4 = sps.log2_max_pic_order_cnt_lsb - 4;
            std_sps.offset_for_non_ref_pic = sps.offset_for_non_ref_pic;
            std_sps.offset_for_top_to_bottom_field = sps.offset_for_top_to_bottom_field;
            std_sps.num_ref_frames_in_pic_order_cnt_cycle = sps.num_ref_frames_in_pic_order_cnt_cycle;
            std_sps.max_num_ref_frames = sps.max_num_ref_frames;
            std_sps.pic_width_in_mbs_minus1 = sps.pic_width_in_mbs - 1;
            std_sps.pic_height_in_map_units_minus1 = sps.pic_height_in_map_units - 1;
            std_sps.frame_crop_left_offset = sps.frame_crop_left;
            std_sps.frame_crop_right_offset = sps.frame_crop_right;
            std_sps.frame_crop_top_offset = sps.frame_crop_top;
            std_sps.frame_crop_bottom_offset = sps.frame_crop_bottom;

            std_sps_list.push_back(std_sps);
        }
    }

    for (size_t i = 0; i < pps_list_.size(); i++) {
        if (pps_list_[i]) {
            const auto& pps = *pps_list_[i];

            StdVideoH264PictureParameterSet std_pps = {};
            std_pps.flags.transform_8x8_mode_flag = pps.transform_8x8_mode_flag ? 1 : 0;
            std_pps.flags.redundant_pic_cnt_present_flag = pps.redundant_pic_cnt_present ? 1 : 0;
            std_pps.flags.constrained_intra_pred_flag = pps.constrained_intra_pred_flag ? 1 : 0;
            std_pps.flags.deblocking_filter_control_present_flag = pps.deblocking_filter_control_present ? 1 : 0;
            std_pps.flags.weighted_pred_flag = pps.weighted_pred_flag ? 1 : 0;
            std_pps.flags.bottom_field_pic_order_in_frame_present_flag = pps.bottom_field_pic_order_in_frame_present ? 1 : 0;
            std_pps.flags.entropy_coding_mode_flag = pps.entropy_coding_mode_flag ? 1 : 0;
            std_pps.flags.pic_scaling_matrix_present_flag = pps.pic_scaling_matrix_present ? 1 : 0;

            std_pps.seq_parameter_set_id = pps.sps_id;
            std_pps.pic_parameter_set_id = pps.pps_id;
            std_pps.num_ref_idx_l0_default_active_minus1 = pps.num_ref_idx_l0_default_active - 1;
            std_pps.num_ref_idx_l1_default_active_minus1 = pps.num_ref_idx_l1_default_active - 1;
            std_pps.weighted_bipred_idc = static_cast<StdVideoH264WeightedBipredIdc>(pps.weighted_bipred_idc);
            std_pps.pic_init_qp_minus26 = pps.pic_init_qp - 26;
            std_pps.pic_init_qs_minus26 = pps.pic_init_qs - 26;
            std_pps.chroma_qp_index_offset = pps.chroma_qp_index_offset;
            std_pps.second_chroma_qp_index_offset = pps.second_chroma_qp_index_offset;

            std_pps_list.push_back(std_pps);
        }
    }

    if (std_sps_list.empty() || std_pps_list.empty()) {
        MLOG_WARN("VkVideo", "No SPS/PPS available for session parameters");
        return false;
    }

    VkVideoDecodeH264SessionParametersAddInfoKHR h264_add_info = {};
    h264_add_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
    h264_add_info.stdSPSCount = static_cast<uint32_t>(std_sps_list.size());
    h264_add_info.pStdSPSs = std_sps_list.data();
    h264_add_info.stdPPSCount = static_cast<uint32_t>(std_pps_list.size());
    h264_add_info.pStdPPSs = std_pps_list.data();

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264_params = {};
    h264_params.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
    h264_params.maxStdSPSCount = 32;
    h264_params.maxStdPPSCount = 256;
    h264_params.pParametersAddInfo = &h264_add_info;

    VkVideoSessionParametersCreateInfoKHR params_info = {};
    params_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    params_info.pNext = &h264_params;
    params_info.videoSession = video_session_;

    VkResult result = vkfn_.CreateVideoSessionParameters(device_, &params_info, nullptr, &session_params_);
    if (result != VK_SUCCESS) {
        MLOG_ERROR("VkVideo", "Failed to create video session parameters: %d", result);
        return false;
    }

    MLOG_INFO("VkVideo", "Video session parameters created: %zu SPS, %zu PPS",
              std_sps_list.size(), std_pps_list.size());

    return true;
}

void VulkanVideoDecoder::destroyVideoSession() {
    if (session_params_ != VK_NULL_HANDLE) {
        vkfn_.DestroyVideoSessionParameters(device_, session_params_, nullptr);
        session_params_ = VK_NULL_HANDLE;
    }

    freeDpbSlots();

    for (auto& mem : session_memory_) {
        if (mem != VK_NULL_HANDLE) {
            vkFreeMemory(device_, mem, nullptr);
        }
    }
    session_memory_.clear();

    if (video_session_ != VK_NULL_HANDLE) {
        vkfn_.DestroyVideoSession(device_, video_session_, nullptr);
        video_session_ = VK_NULL_HANDLE;
    }
}

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

        // Create image view
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = slot.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        // Multi-planar NV12 format requires plane aspect bits (not COLOR_BIT)
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
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

DecodeResult VulkanVideoDecoder::decodeSlice(const uint8_t* nal_data, size_t nal_size, int64_t pts) {
    DecodeResult result;
    result.pts = pts;

    if (video_session_ == VK_NULL_HANDLE || session_params_ == VK_NULL_HANDLE) {
        result.error_message = "Video session not ready";
        errors_count_++;
        return result;
    }

    // Acquire frame resources for async decode
    uint32_t frame_index = acquireFrameResources();
    auto& frame_res = frame_resources_[frame_index];

    // Parse slice header to determine reference picture requirements
    H264SliceHeader slice_header;
    H264Parser parser;

    // Get NAL unit type and parse slice header
    uint8_t nal_type = nal_data[0] & 0x1F;
    bool is_idr = (nal_type == static_cast<uint8_t>(NalUnitType::SLICE_IDR));

    // Remove emulation prevention bytes for parsing
    std::vector<uint8_t> rbsp = parser.removeEmulationPrevention(nal_data, nal_size);

    // Find active PPS and SPS
    if (active_pps_id_ < 0 || active_pps_id_ >= (int)pps_list_.size() || !pps_list_[active_pps_id_]) {
        // Try to parse slice header to get PPS ID
        if (rbsp.size() > 1) {
            BitstreamReader br(rbsp.data() + 1, rbsp.size() - 1);  // Skip NAL header
            br.readUE();  // first_mb_in_slice
            br.readUE();  // slice_type
            uint8_t pps_id = static_cast<uint8_t>(br.readUE());
            if (pps_id < pps_list_.size() && pps_list_[pps_id]) {
                active_pps_id_ = pps_id;
                active_sps_id_ = pps_list_[pps_id]->sps_id;
            }
        }
    }

    if (active_pps_id_ < 0 || !pps_list_[active_pps_id_] ||
        active_sps_id_ < 0 || !sps_list_[active_sps_id_]) {
        result.error_message = "No active SPS/PPS";
        errors_count_++;
        return result;
    }

    const H264SPS& sps = *sps_list_[active_sps_id_];
    const H264PPS& pps = *pps_list_[active_pps_id_];

    // Parse full slice header
    parser.parseSliceHeader(rbsp.data() + 1, rbsp.size() - 1, sps, pps, is_idr, slice_header);

    // Resize frame bitstream buffer if needed
    if (nal_size > frame_res.bitstream_buffer_size) {
        size_t new_size = nal_size * 2;
        if (!createFrameBitstreamBuffer(frame_index, new_size)) {
            result.error_message = "Failed to resize bitstream buffer";
            errors_count_++;
            releaseFrameResources(frame_index);
            return result;
        }
    }

    // Copy NAL data to frame's bitstream buffer
    memcpy(frame_res.bitstream_mapped, nal_data, nal_size);

    // Acquire output DPB slot
    int32_t output_slot = acquireDpbSlot();
    if (output_slot < 0) {
        result.error_message = "No DPB slot available";
        errors_count_++;
        return result;
    }

    auto& slot = dpb_slots_[output_slot];

    // Build reference picture list from DPB
    std::vector<VkVideoReferenceSlotInfoKHR> ref_slots;
    std::vector<VkVideoPictureResourceInfoKHR> ref_pics;
    std::vector<VkVideoDecodeH264DpbSlotInfoKHR> h264_dpb_slots;
    std::vector<StdVideoDecodeH264ReferenceInfo> std_ref_infos;

    // Reserve space to prevent reallocation
    ref_slots.reserve(dpb_slots_.size());
    ref_pics.reserve(dpb_slots_.size());
    h264_dpb_slots.reserve(dpb_slots_.size());
    std_ref_infos.reserve(dpb_slots_.size());

    for (size_t i = 0; i < dpb_slots_.size(); i++) {
        if (i == static_cast<size_t>(output_slot)) continue;  // Skip output slot
        if (!dpb_slots_[i].is_reference || !dpb_slots_[i].in_use) continue;

        // Build StdVideo reference info
        StdVideoDecodeH264ReferenceInfo std_ref = {};
        std_ref.FrameNum = static_cast<uint16_t>(dpb_slots_[i].frame_num);
        std_ref.PicOrderCnt[0] = dpb_slots_[i].poc;
        std_ref.PicOrderCnt[1] = dpb_slots_[i].poc;
        std_ref.flags.top_field_flag = 0;
        std_ref.flags.bottom_field_flag = 0;
        std_ref.flags.used_for_long_term_reference = dpb_slots_[i].is_long_term ? 1 : 0;
        std_ref.flags.is_non_existing = 0;
        std_ref_infos.push_back(std_ref);

        // Build Vulkan DPB slot info
        VkVideoDecodeH264DpbSlotInfoKHR h264_slot = {};
        h264_slot.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
        h264_slot.pStdReferenceInfo = &std_ref_infos.back();
        h264_dpb_slots.push_back(h264_slot);

        // Build picture resource
        VkVideoPictureResourceInfoKHR pic = {};
        pic.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        pic.codedOffset = {0, 0};
        pic.codedExtent = {current_width_, current_height_};
        pic.baseArrayLayer = 0;
        pic.imageViewBinding = dpb_slots_[i].view;
        ref_pics.push_back(pic);

        // Build reference slot
        VkVideoReferenceSlotInfoKHR ref_slot = {};
        ref_slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        ref_slot.pNext = &h264_dpb_slots.back();
        ref_slot.slotIndex = static_cast<int32_t>(i);
        ref_slot.pPictureResource = &ref_pics.back();
        ref_slots.push_back(ref_slot);
    }

    // Record decode command
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkCommandBuffer cmd_buffer = frame_res.cmd_buffer;
    vkResetCommandBuffer(cmd_buffer, 0);
    vkBeginCommandBuffer(cmd_buffer, &begin_info);

    // Begin video coding scope
    VkVideoBeginCodingInfoKHR begin_coding = {};
    begin_coding.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    begin_coding.videoSession = video_session_;
    begin_coding.videoSessionParameters = session_params_;
    begin_coding.referenceSlotCount = static_cast<uint32_t>(ref_slots.size());
    begin_coding.pReferenceSlots = ref_slots.empty() ? nullptr : ref_slots.data();

    vkfn_.CmdBeginVideoCoding(cmd_buffer, &begin_coding);

    // Build StdVideoDecodeH264PictureInfo
    StdVideoDecodeH264PictureInfo std_pic_info = {};
    std_pic_info.flags.field_pic_flag = slice_header.field_pic_flag ? 1 : 0;
    std_pic_info.flags.is_intra = (slice_header.slice_type == 2 || slice_header.slice_type == 7) ? 1 : 0;
    std_pic_info.flags.IdrPicFlag = is_idr ? 1 : 0;
    std_pic_info.flags.bottom_field_flag = slice_header.bottom_field_flag ? 1 : 0;
    std_pic_info.flags.is_reference = 1;  // Assume all decoded frames can be used as reference
    std_pic_info.flags.complementary_field_pair = 0;
    std_pic_info.seq_parameter_set_id = static_cast<uint8_t>(active_sps_id_);
    std_pic_info.pic_parameter_set_id = static_cast<uint8_t>(active_pps_id_);
    std_pic_info.frame_num = slice_header.frame_num;
    std_pic_info.idr_pic_id = slice_header.idr_pic_id;
    uint8_t nal_ref_idc = (nal_data[0] >> 5) & 0x03;
    std_pic_info.PicOrderCnt[0] = calculatePOC(slice_header, sps, is_idr, nal_ref_idc);
    std_pic_info.PicOrderCnt[1] = std_pic_info.PicOrderCnt[0];

    // Build VkVideoDecodeH264PictureInfoKHR
    uint32_t slice_offset = 0;  // Single slice at offset 0
    VkVideoDecodeH264PictureInfoKHR h264_pic_info = {};
    h264_pic_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR;
    h264_pic_info.pStdPictureInfo = &std_pic_info;
    h264_pic_info.sliceCount = 1;
    h264_pic_info.pSliceOffsets = &slice_offset;

    // Output picture resource
    VkVideoPictureResourceInfoKHR output_pic = {};
    output_pic.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    output_pic.codedOffset = {0, 0};
    output_pic.codedExtent = {current_width_, current_height_};
    output_pic.baseArrayLayer = 0;
    output_pic.imageViewBinding = slot.view;

    // Output DPB slot info
    StdVideoDecodeH264ReferenceInfo output_std_ref = {};
    output_std_ref.FrameNum = slice_header.frame_num;
    output_std_ref.PicOrderCnt[0] = std_pic_info.PicOrderCnt[0];
    output_std_ref.PicOrderCnt[1] = std_pic_info.PicOrderCnt[1];
    output_std_ref.flags.top_field_flag = 0;
    output_std_ref.flags.bottom_field_flag = 0;
    output_std_ref.flags.used_for_long_term_reference = 0;
    output_std_ref.flags.is_non_existing = 0;

    VkVideoDecodeH264DpbSlotInfoKHR output_h264_slot = {};
    output_h264_slot.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
    output_h264_slot.pStdReferenceInfo = &output_std_ref;

    VkVideoReferenceSlotInfoKHR setup_slot = {};
    setup_slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    setup_slot.pNext = &output_h264_slot;
    setup_slot.slotIndex = output_slot;
    setup_slot.pPictureResource = &output_pic;

    // Build decode info
    // Align srcBufferRange to minBitstreamBufferSizeAlignment
    VkDeviceSize size_alignment = capabilities_.minBitstreamBufferSizeAlignment;
    if (size_alignment == 0) size_alignment = 1;
    VkDeviceSize aligned_nal_size = ((nal_size + size_alignment - 1) / size_alignment) * size_alignment;
    // Ensure we don't exceed buffer size
    if (aligned_nal_size > frame_res.bitstream_buffer_size) {
        aligned_nal_size = frame_res.bitstream_buffer_size;
    }

    VkVideoDecodeInfoKHR decode_info = {};
    decode_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
    decode_info.pNext = &h264_pic_info;
    decode_info.srcBuffer = frame_res.bitstream_buffer;
    decode_info.srcBufferOffset = 0;  // Always 0, aligned by buffer base
    decode_info.srcBufferRange = aligned_nal_size;
    decode_info.dstPictureResource = output_pic;
    decode_info.pSetupReferenceSlot = &setup_slot;
    decode_info.referenceSlotCount = static_cast<uint32_t>(ref_slots.size());
    decode_info.pReferenceSlots = ref_slots.empty() ? nullptr : ref_slots.data();

    vkfn_.CmdDecodeVideo(cmd_buffer, &decode_info);

    // End video coding scope
    VkVideoEndCodingInfoKHR end_coding = {};
    end_coding.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;

    vkfn_.CmdEndVideoCoding(cmd_buffer, &end_coding);

    vkEndCommandBuffer(cmd_buffer);

    // Submit to video decode queue with timeline semaphore
    timeline_value_++;
    uint64_t signal_value = timeline_value_;
    frame_res.timeline_value = signal_value;

    VkTimelineSemaphoreSubmitInfo timeline_submit = {};
    timeline_submit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_submit.signalSemaphoreValueCount = 1;
    timeline_submit.pSignalSemaphoreValues = &signal_value;

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_submit;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &timeline_semaphore_;

    VkResult vk_result = vkQueueSubmit(video_queue_, 1, &submit_info, VK_NULL_HANDLE);
    if (vk_result != VK_SUCCESS) {
        result.error_message = "Failed to submit decode command";
        errors_count_++;
        releaseFrameResources(frame_index);
        releaseDpbSlot(output_slot);
        return result;
    }

    // Async decode: Only wait if not in async mode, otherwise proceed
    if (!config_.async_decode) {
        // Sync mode: Wait for decode to complete
        VkSemaphoreWaitInfo wait_info = {};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &timeline_semaphore_;
        wait_info.pValues = &signal_value;

        vk_result = vkWaitSemaphores(device_, &wait_info, UINT64_MAX);
        if (vk_result != VK_SUCCESS) {
            result.error_message = "Failed to wait for decode completion";
            errors_count_++;
            releaseFrameResources(frame_index);
            return result;
        }
        releaseFrameResources(frame_index);
    }
    // In async mode, frame resources are released when the next frame is acquired

    // Update DPB slot state
    slot.frame_num = slice_header.frame_num;
    slot.poc = std_pic_info.PicOrderCnt[0];
    slot.is_reference = true;
    slot.is_long_term = slice_header.long_term_reference_flag && is_idr;

    // For IDR, handle DPB clearing according to flags
    if (is_idr) {
        // If no_output_of_prior_pics_flag is set, discard prior pictures without output
        // Otherwise, output them first via the reorder buffer (handled by outputReorderedFrames)
        if (slice_header.no_output_of_prior_pics_flag) {
            // Discard all pending frames without output
            reorder_buffer_.clear();
            MLOG_INFO("VkVideo", "IDR with no_output_of_prior_pics: discarding %zu buffered frames",
                      reorder_buffer_.size());
        }

        // Clear all reference frames from DPB
        for (size_t i = 0; i < dpb_slots_.size(); i++) {
            if (i != static_cast<size_t>(output_slot) && dpb_slots_[i].is_reference) {
                dpb_slots_[i].is_reference = false;
                dpb_slots_[i].in_use = false;
            }
        }
        prev_frame_num_ = 0;
        prev_poc_ = 0;
        last_output_poc_ = INT32_MIN;  // Reset reorder output tracking
    }

    // Apply reference picture marking (MMCO / sliding window)
    applyRefPicMarking(slice_header, is_idr, output_slot);

    prev_frame_num_ = slice_header.frame_num;
    prev_poc_ = slot.poc;

    // Build result
    result.success = true;
    result.output_image = slot.image;
    result.output_view = slot.view;
    result.width = current_width_;
    result.height = current_height_;
    result.poc = slot.poc;

    frames_decoded_++;

    if (frames_decoded_ <= 5 || frames_decoded_ % 100 == 0) {
        MLOG_INFO("VkVideo", "Decoded frame #%llu: %dx%d, POC=%d, refs=%zu",
                  (unsigned long long)frames_decoded_, current_width_, current_height_,
                  slot.poc, ref_slots.size());
    }

    // Add to reorder buffer for B-frame display order output
    PendingFrame pending;
    pending.dpb_slot = output_slot;
    pending.poc = slot.poc;
    pending.pts = pts;
    pending.output_ready = true;
    reorder_buffer_.push_back(pending);

    // Output frames in display order
    outputReorderedFrames(is_idr);  // IDR flushes all pending frames

    return result;
}

int32_t VulkanVideoDecoder::calculatePOC(const H264SliceHeader& header, const H264SPS& sps, bool is_idr, uint8_t nal_ref_idc) {
    int32_t poc = 0;

    switch (sps.pic_order_cnt_type) {
        case 0: {
            // POC type 0: Uses pic_order_cnt_lsb and delta_pic_order_cnt_bottom
            // Based on ITU-T H.264 section 8.2.1.1

            int32_t max_poc_lsb = 1 << sps.log2_max_pic_order_cnt_lsb;
            int32_t poc_lsb = static_cast<int32_t>(header.pic_order_cnt_lsb);
            int32_t poc_msb;

            if (is_idr) {
                // IDR picture resets POC
                poc_msb = 0;
                prev_poc_msb_ = 0;
                prev_poc_lsb_ = 0;
            } else {
                // Calculate POC MSB based on wrap-around detection
                if (poc_lsb < prev_poc_lsb_ && (prev_poc_lsb_ - poc_lsb) >= (max_poc_lsb / 2)) {
                    // POC LSB wrapped around (increased)
                    poc_msb = prev_poc_msb_ + max_poc_lsb;
                } else if (poc_lsb > prev_poc_lsb_ && (poc_lsb - prev_poc_lsb_) > (max_poc_lsb / 2)) {
                    // POC LSB wrapped around (decreased - rare)
                    poc_msb = prev_poc_msb_ - max_poc_lsb;
                } else {
                    poc_msb = prev_poc_msb_;
                }
            }

            // TopFieldOrderCnt
            poc = poc_msb + poc_lsb;

            // Update state for next frame (only for reference pictures per H.264 8.2.1.1)
            if (nal_ref_idc != 0) {
                prev_poc_msb_ = poc_msb;
                prev_poc_lsb_ = poc_lsb;
            }
            break;
        }

        case 1: {
            // POC type 1: Uses frame_num and delta_pic_order_cnt
            // Based on ITU-T H.264 section 8.2.1.2

            int32_t max_frame_num = 1 << sps.log2_max_frame_num;
            int32_t frame_num = static_cast<int32_t>(header.frame_num);

            // Calculate frame_num_offset
            if (is_idr) {
                frame_num_offset_ = 0;
            } else if (prev_frame_num_ > frame_num) {
                // frame_num wrapped around
                frame_num_offset_ = prev_frame_num_offset_ + max_frame_num;
            } else {
                frame_num_offset_ = prev_frame_num_offset_;
            }

            // Calculate absFrameNum
            int32_t abs_frame_num = 0;
            if (sps.num_ref_frames_in_pic_order_cnt_cycle != 0) {
                abs_frame_num = frame_num_offset_ + frame_num;
            }

            // Calculate expectedPicOrderCnt
            int32_t expected_poc = 0;
            if (abs_frame_num > 0) {
                // Calculate expected_delta_per_poc_cycle
                int32_t expected_delta_per_poc_cycle = 0;
                for (int i = 0; i < sps.num_ref_frames_in_pic_order_cnt_cycle; i++) {
                    expected_delta_per_poc_cycle += sps.offset_for_ref_frame[i];
                }

                int32_t poc_cycle_cnt = (abs_frame_num - 1) / sps.num_ref_frames_in_pic_order_cnt_cycle;
                int32_t frame_num_in_poc_cycle = (abs_frame_num - 1) % sps.num_ref_frames_in_pic_order_cnt_cycle;

                expected_poc = poc_cycle_cnt * expected_delta_per_poc_cycle;
                for (int i = 0; i <= frame_num_in_poc_cycle; i++) {
                    expected_poc += sps.offset_for_ref_frame[i];
                }
            }

            // Apply offset_for_non_ref_pic for non-reference pictures
            // (We assume reference picture here)

            // TopFieldOrderCnt
            poc = expected_poc + header.delta_pic_order_cnt[0];

            // Update state
            prev_frame_num_offset_ = frame_num_offset_;
            break;
        }

        case 2: {
            // POC type 2: POC derived directly from frame_num
            // Based on ITU-T H.264 section 8.2.1.3

            int32_t max_frame_num = 1 << sps.log2_max_frame_num;
            int32_t frame_num = static_cast<int32_t>(header.frame_num);

            // Calculate frame_num_offset
            if (is_idr) {
                frame_num_offset_ = 0;
            } else if (prev_frame_num_ > frame_num) {
                // frame_num wrapped around
                frame_num_offset_ = prev_frame_num_offset_ + max_frame_num;
            } else {
                frame_num_offset_ = prev_frame_num_offset_;
            }

            // Calculate tempPicOrderCnt
            int32_t temp_poc;
            if (is_idr) {
                temp_poc = 0;
            } else {
                temp_poc = 2 * (frame_num_offset_ + frame_num);
                // For non-reference pictures, subtract 1
                // (We assume reference picture here)
            }

            poc = temp_poc;

            // Update state
            prev_frame_num_offset_ = frame_num_offset_;
            break;
        }

        default:
            // Unknown POC type, fallback to frame_num
            poc = static_cast<int32_t>(header.frame_num);
            break;
    }

    return poc;
}

void VulkanVideoDecoder::applyRefPicMarking(const H264SliceHeader& header, bool is_idr, int32_t current_slot) {
    // Get max_num_ref_frames from SPS
    int32_t max_refs = 16;  // Default max
    if (active_sps_id_ >= 0 && active_sps_id_ < static_cast<int32_t>(sps_list_.size()) &&
        sps_list_[active_sps_id_]) {
        max_refs = sps_list_[active_sps_id_]->max_num_ref_frames;
    }

    // Helper: find DPB slot by frame_num (short-term reference)
    auto findSlotByFrameNum = [this](int32_t frame_num) -> int32_t {
        for (size_t i = 0; i < dpb_slots_.size(); i++) {
            if (dpb_slots_[i].in_use && dpb_slots_[i].is_reference &&
                !dpb_slots_[i].is_long_term && dpb_slots_[i].frame_num == frame_num) {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    };

    // Helper: find DPB slot by long_term_pic_num
    auto findSlotByLongTermPicNum = [this](int32_t lt_pic_num) -> int32_t {
        for (size_t i = 0; i < dpb_slots_.size(); i++) {
            if (dpb_slots_[i].in_use && dpb_slots_[i].is_reference &&
                dpb_slots_[i].is_long_term && dpb_slots_[i].frame_num == lt_pic_num) {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    };

    // max_long_term_frame_idx_ is now an instance variable (not static)
    // to support multiple VulkanVideoDecoder instances without state collision

    if (is_idr) {
        // IDR picture: mark all references as unused, reset state
        max_long_term_frame_idx_ = -1;

        if (header.long_term_reference_flag) {
            // Mark current picture as long-term reference with frame_idx = 0
            if (current_slot >= 0 && current_slot < static_cast<int32_t>(dpb_slots_.size())) {
                dpb_slots_[current_slot].is_long_term = true;
                max_long_term_frame_idx_ = 0;
                MLOG_INFO("VkVideo", "IDR marked as long-term reference: slot=%d", current_slot);
            }
        }
        // no_output_of_prior_pics_flag is handled elsewhere (DPB clear)
    } else if (header.adaptive_ref_pic_marking_mode_flag) {
        // Execute MMCO commands in order
        for (const auto& cmd : header.mmco_commands) {
            switch (cmd.operation) {
                case 1: {
                    // Mark short-term picture as "unused for reference"
                    // picNumX = CurrPicNum - (difference_of_pic_nums_minus1 + 1)
                    int32_t picNumX = header.frame_num - (cmd.difference_of_pic_nums_minus1 + 1);
                    int32_t slot = findSlotByFrameNum(picNumX);
                    if (slot >= 0) {
                        dpb_slots_[slot].is_reference = false;
                        MLOG_INFO("VkVideo", "MMCO 1: short-term frame_num=%d (slot=%d) -> unused", picNumX, slot);
                    }
                    break;
                }
                case 2: {
                    // Mark long-term picture as "unused for reference"
                    int32_t slot = findSlotByLongTermPicNum(cmd.long_term_pic_num);
                    if (slot >= 0) {
                        dpb_slots_[slot].is_reference = false;
                        dpb_slots_[slot].is_long_term = false;
                        MLOG_INFO("VkVideo", "MMCO 2: long-term pic_num=%d (slot=%d) -> unused",
                                  cmd.long_term_pic_num, slot);
                    }
                    break;
                }
                case 3: {
                    // Assign long_term_frame_idx to a short-term picture
                    int32_t picNumX = header.frame_num - (cmd.difference_of_pic_nums_minus1 + 1);
                    int32_t slot = findSlotByFrameNum(picNumX);
                    if (slot >= 0) {
                        // First, unmark any existing LT ref with same frame_idx
                        for (auto& dpb : dpb_slots_) {
                            if (dpb.is_long_term && dpb.frame_num == static_cast<int32_t>(cmd.long_term_frame_idx)) {
                                dpb.is_reference = false;
                                dpb.is_long_term = false;
                            }
                        }
                        dpb_slots_[slot].is_long_term = true;
                        dpb_slots_[slot].frame_num = cmd.long_term_frame_idx;
                        MLOG_INFO("VkVideo", "MMCO 3: short-term frame_num=%d -> long-term idx=%d (slot=%d)",
                                  picNumX, cmd.long_term_frame_idx, slot);
                    }
                    break;
                }
                case 4: {
                    // Specify max long-term frame index
                    max_long_term_frame_idx_ = static_cast<int32_t>(cmd.max_long_term_frame_idx_plus1) - 1;
                    // Mark all LT refs with frame_idx > max as unused
                    for (auto& dpb : dpb_slots_) {
                        if (dpb.is_long_term && dpb.frame_num > max_long_term_frame_idx_) {
                            dpb.is_reference = false;
                            dpb.is_long_term = false;
                            MLOG_INFO("VkVideo", "MMCO 4: LT frame_idx=%d > max=%d -> unused",
                                      dpb.frame_num, max_long_term_frame_idx_);
                        }
                    }
                    // max_long_term_frame_idx_plus1 == 0 means no LT refs allowed
                    if (cmd.max_long_term_frame_idx_plus1 == 0) {
                        max_long_term_frame_idx_ = -1;
                    }
                    break;
                }
                case 5: {
                    // Mark all reference pictures as "unused for reference"
                    for (auto& dpb : dpb_slots_) {
                        if (dpb.is_reference) {
                            dpb.is_reference = false;
                            dpb.is_long_term = false;
                        }
                    }
                    max_long_term_frame_idx_ = -1;
                    MLOG_INFO("VkVideo", "MMCO 5: all references marked unused");
                    break;
                }
                case 6: {
                    // Assign long_term_frame_idx to current picture
                    if (current_slot >= 0) {
                        // First, unmark any existing LT ref with same frame_idx
                        for (auto& dpb : dpb_slots_) {
                            if (dpb.is_long_term && dpb.frame_num == static_cast<int32_t>(cmd.long_term_frame_idx)) {
                                dpb.is_reference = false;
                                dpb.is_long_term = false;
                            }
                        }
                        dpb_slots_[current_slot].is_long_term = true;
                        dpb_slots_[current_slot].frame_num = cmd.long_term_frame_idx;
                        MLOG_INFO("VkVideo", "MMCO 6: current -> long-term idx=%d (slot=%d)",
                                  cmd.long_term_frame_idx, current_slot);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    } else {
        // Sliding window reference picture marking
        // Mark oldest short-term reference as unused when DPB is full
        int32_t num_short_term = 0;
        int32_t oldest_short_term_slot = -1;
        int32_t oldest_frame_num = INT32_MAX;

        for (size_t i = 0; i < dpb_slots_.size(); i++) {
            if (dpb_slots_[i].is_reference && dpb_slots_[i].in_use && !dpb_slots_[i].is_long_term) {
                num_short_term++;
                if (dpb_slots_[i].frame_num < oldest_frame_num) {
                    oldest_frame_num = dpb_slots_[i].frame_num;
                    oldest_short_term_slot = static_cast<int32_t>(i);
                }
            }
        }

        // If we have too many short-term references, remove the oldest
        if (num_short_term > max_refs && oldest_short_term_slot >= 0) {
            dpb_slots_[oldest_short_term_slot].is_reference = false;
            MLOG_INFO("VkVideo", "Sliding window: removed short-term ref slot=%d, frame_num=%d",
                      oldest_short_term_slot, oldest_frame_num);
        }
    }
}

void VulkanVideoDecoder::outputReorderedFrames(bool flush_all) {
    if (reorder_buffer_.empty()) {
        return;
    }

    // Sort by POC for display order
    std::sort(reorder_buffer_.begin(), reorder_buffer_.end(),
              [](const PendingFrame& a, const PendingFrame& b) {
                  return a.poc < b.poc;
              });

    // Output frames that are ready (POC order, no gaps)
    while (!reorder_buffer_.empty()) {
        auto& front = reorder_buffer_.front();

        // Check if this frame can be output
        // - If flush_all (IDR received), output everything
        // - Otherwise, only output if POC is next in sequence
        bool can_output = flush_all ||
                          (front.poc == last_output_poc_ + 1) ||
                          (last_output_poc_ == INT32_MIN) ||
                          (reorder_buffer_.size() >= MAX_REORDER_BUFFER);

        if (!can_output) {
            // Check if we have enough frames buffered to be confident
            // about the next output (look for reference frames)
            int32_t min_poc = front.poc;
            bool found_higher_ref = false;
            for (const auto& pf : reorder_buffer_) {
                if (pf.poc > min_poc) {
                    found_higher_ref = true;
                    break;
                }
            }
            can_output = found_higher_ref;
        }

        if (!can_output) {
            break;
        }

        // Output this frame via callback
        if (frame_callback_ && front.dpb_slot >= 0 &&
            front.dpb_slot < static_cast<int32_t>(dpb_slots_.size())) {
            auto& slot = dpb_slots_[front.dpb_slot];
            if (slot.in_use) {
                frame_callback_(slot.image, slot.view,
                               current_width_, current_height_, front.pts);
            }
        }

        last_output_poc_ = front.poc;
        reorder_buffer_.erase(reorder_buffer_.begin());
    }

    // Limit buffer size to prevent unbounded growth
    while (reorder_buffer_.size() > MAX_REORDER_BUFFER) {
        auto& oldest = reorder_buffer_.front();
        if (frame_callback_ && oldest.dpb_slot >= 0 &&
            oldest.dpb_slot < static_cast<int32_t>(dpb_slots_.size())) {
            auto& slot = dpb_slots_[oldest.dpb_slot];
            if (slot.in_use) {
                frame_callback_(slot.image, slot.view,
                               current_width_, current_height_, oldest.pts);
            }
        }
        last_output_poc_ = oldest.poc;
        reorder_buffer_.erase(reorder_buffer_.begin());
    }
}

} // namespace mirage::video
