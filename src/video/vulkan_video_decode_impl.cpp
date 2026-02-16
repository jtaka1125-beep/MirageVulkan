// =============================================================================
// MirageSystem - Vulkan Video Decode Implementation
// =============================================================================
// Split from vulkan_video_decoder.cpp for maintainability
// Contains: decodeSlice, POC calculation, MMCO, frame reordering
// =============================================================================

#include "vulkan_video_decoder.hpp"
#include "../mirage_log.hpp"

#include <cstring>
#include <algorithm>

namespace mirage::video {

DecodeResult VulkanVideoDecoder::decodeSlice(const uint8_t* nal_data, size_t nal_size, int64_t pts) {
    DecodeResult result;
    result.pts = pts;

    if (video_session_ == VK_NULL_HANDLE || session_params_ == VK_NULL_HANDLE) {
        result.error_message = "Video session not ready";
        errors_count_++;
        return result;
    }

    // Skip start code to find NAL header
    // nal_data may include start code (00 00 01 or 00 00 00 01)
    const uint8_t* nal_header = nal_data;
    size_t header_offset = 0;
    if (nal_size >= 4 && nal_data[0] == 0 && nal_data[1] == 0) {
        if (nal_data[2] == 1) {
            header_offset = 3;
        } else if (nal_size >= 5 && nal_data[2] == 0 && nal_data[3] == 1) {
            header_offset = 4;
        }
    }
    nal_header = nal_data + header_offset;
    size_t nal_payload_size = nal_size - header_offset;

    // Acquire frame resources for async decode
    uint32_t frame_index = acquireFrameResources();
    auto& frame_res = frame_resources_[frame_index];

    // Parse slice header to determine reference picture requirements
    H264SliceHeader slice_header;
    H264Parser parser;

    // Get NAL unit type and parse slice header (from NAL header, not start code)
    uint8_t nal_type = nal_header[0] & 0x1F;
    bool is_idr = (nal_type == static_cast<uint8_t>(NalUnitType::SLICE_IDR));

    // Remove emulation prevention bytes for parsing (from NAL header)
    std::vector<uint8_t> rbsp = parser.removeEmulationPrevention(nal_header, nal_payload_size);

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
    uint8_t nal_ref_idc = (nal_header[0] >> 5) & 0x03;
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
