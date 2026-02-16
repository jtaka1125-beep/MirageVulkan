// =============================================================================
// MirageSystem - Vulkan Video Session Management
// =============================================================================
// Split from vulkan_video_decoder.cpp for maintainability
// Contains: createVideoSession, createVideoSessionParameters, destroyVideoSession
// =============================================================================

#include "vulkan_video_decoder.hpp"
#include "../mirage_log.hpp"

#include <cstring>
#include <algorithm>

namespace mirage::video {

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

} // namespace mirage::video
