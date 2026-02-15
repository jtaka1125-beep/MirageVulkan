// =============================================================================
// MirageSystem - H.264 Bitstream Parser Implementation
// =============================================================================

#include "h264_parser.hpp"

namespace mirage::video {

// =============================================================================
// H264Parser Implementation
// =============================================================================

std::vector<NalUnit> H264Parser::parseAnnexB(const uint8_t* data, size_t size) {
    std::vector<NalUnit> nals;
    if (!data || size < 4) return nals;

    size_t pos = 0;

    // Find first start code
    while (pos + 3 < size) {
        if (data[pos] == 0 && data[pos + 1] == 0) {
            if (data[pos + 2] == 1) {
                pos += 3;
                break;
            } else if (pos + 3 < size && data[pos + 2] == 0 && data[pos + 3] == 1) {
                pos += 4;
                break;
            }
        }
        pos++;
    }

    while (pos < size) {
        // NAL header
        uint8_t header = data[pos];
        uint8_t forbidden_bit = (header >> 7) & 1;
        if (forbidden_bit) {
            pos++;
            continue;
        }

        NalUnit nal;
        nal.nal_ref_idc = (header >> 5) & 0x03;
        nal.nal_unit_type = header & 0x1F;
        nal.data = data + pos - (data[pos - 1] == 1 ? 3 : 4);

        // Find end of NAL (next start code or end of data)
        size_t nal_start = pos + 1;  // After NAL header
        size_t nal_end = size;

        for (size_t i = nal_start; i + 2 < size; i++) {
            if (data[i] == 0 && data[i + 1] == 0 &&
                (data[i + 2] == 1 || (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1))) {
                nal_end = i;
                break;
            }
        }

        nal.rbsp_data = data + nal_start;
        nal.rbsp_size = nal_end - nal_start;
        nal.size = nal_end - (nal.data - data);

        nals.push_back(nal);

        // Move to next NAL - find next start code
        pos = nal_end;
        bool found_next = false;
        while (pos + 2 < size) {
            if (data[pos] == 0 && data[pos + 1] == 0) {
                if (data[pos + 2] == 1) {
                    pos += 3;
                    found_next = true;
                    break;
                } else if (pos + 3 < size && data[pos + 2] == 0 && data[pos + 3] == 1) {
                    pos += 4;
                    found_next = true;
                    break;
                }
            }
            pos++;
        }
        if (!found_next) break;
    }

    return nals;
}

std::vector<uint8_t> H264Parser::removeEmulationPrevention(const uint8_t* data, size_t size) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(size);

    for (size_t i = 0; i < size; i++) {
        // Check for emulation prevention byte pattern: 0x00 0x00 0x03
        if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0x03) {
            rbsp.push_back(data[i]);
            rbsp.push_back(data[i + 1]);
            i += 2;  // Skip 0x03
        } else {
            rbsp.push_back(data[i]);
        }
    }

    return rbsp;
}

bool H264Parser::parseSPS(const uint8_t* rbsp, size_t size, H264SPS& sps) {
    auto clean_rbsp = removeEmulationPrevention(rbsp, size);
    BitstreamReader br(clean_rbsp.data(), clean_rbsp.size());

    sps.profile_idc = br.readBits(8);

    // Constraint flags (6 bits) + reserved (2 bits)
    br.skipBits(8);

    sps.level_idc = br.readBits(8);
    sps.sps_id = br.readUE();

    // High profile extensions
    if (sps.profile_idc == 100 || sps.profile_idc == 110 ||
        sps.profile_idc == 122 || sps.profile_idc == 244 ||
        sps.profile_idc == 44 || sps.profile_idc == 83 ||
        sps.profile_idc == 86 || sps.profile_idc == 118 ||
        sps.profile_idc == 128 || sps.profile_idc == 138 ||
        sps.profile_idc == 139 || sps.profile_idc == 134 ||
        sps.profile_idc == 135) {

        sps.chroma_format_idc = br.readUE();
        if (sps.chroma_format_idc == 3) {
            br.readFlag();  // separate_colour_plane_flag
        }

        sps.bit_depth_luma = br.readUE() + 8;
        sps.bit_depth_chroma = br.readUE() + 8;

        br.readFlag();  // qpprime_y_zero_transform_bypass_flag

        bool seq_scaling_matrix_present = br.readFlag();
        if (seq_scaling_matrix_present) {
            int scaling_list_count = (sps.chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < scaling_list_count; i++) {
                bool present = br.readFlag();
                if (present) {
                    parseScalingList(br, (i < 6) ? 16 : 64);
                }
            }
        }
    }

    sps.log2_max_frame_num = br.readUE() + 4;
    sps.pic_order_cnt_type = br.readUE();

    if (sps.pic_order_cnt_type == 0) {
        sps.log2_max_pic_order_cnt_lsb = br.readUE() + 4;
    } else if (sps.pic_order_cnt_type == 1) {
        sps.delta_pic_order_always_zero_flag = br.readFlag();
        sps.offset_for_non_ref_pic = br.readSE();
        sps.offset_for_top_to_bottom_field = br.readSE();
        sps.num_ref_frames_in_pic_order_cnt_cycle = br.readUE();
        sps.offset_for_ref_frame.resize(sps.num_ref_frames_in_pic_order_cnt_cycle);
        for (int i = 0; i < sps.num_ref_frames_in_pic_order_cnt_cycle; i++) {
            sps.offset_for_ref_frame[i] = br.readSE();
        }
    }

    sps.max_num_ref_frames = br.readUE();
    sps.gaps_in_frame_num_allowed = br.readFlag();
    sps.pic_width_in_mbs = br.readUE() + 1;
    sps.pic_height_in_map_units = br.readUE() + 1;

    sps.frame_mbs_only_flag = br.readFlag();
    if (!sps.frame_mbs_only_flag) {
        br.readFlag();  // mb_adaptive_frame_field_flag
    }

    sps.direct_8x8_inference_flag = br.readFlag();

    sps.frame_cropping_flag = br.readFlag();
    if (sps.frame_cropping_flag) {
        sps.frame_crop_left = br.readUE();
        sps.frame_crop_right = br.readUE();
        sps.frame_crop_top = br.readUE();
        sps.frame_crop_bottom = br.readUE();
    }

    sps.vui_parameters_present = br.readFlag();
    if (sps.vui_parameters_present) {
        parseVuiParameters(br, sps);
    }

    return true;
}

bool H264Parser::parsePPS(const uint8_t* rbsp, size_t size, H264PPS& pps) {
    auto clean_rbsp = removeEmulationPrevention(rbsp, size);
    BitstreamReader br(clean_rbsp.data(), clean_rbsp.size());

    pps.pps_id = br.readUE();
    pps.sps_id = br.readUE();
    pps.entropy_coding_mode_flag = br.readFlag();
    pps.bottom_field_pic_order_in_frame_present = br.readFlag();

    pps.num_slice_groups = br.readUE() + 1;
    if (pps.num_slice_groups > 1) {
        br.readUE();  // slice_group_map_type
        // Skip slice group parameters (complex, rarely used)
        // For simplicity, we only support num_slice_groups == 1
        return false;
    }

    pps.num_ref_idx_l0_default_active = br.readUE() + 1;
    pps.num_ref_idx_l1_default_active = br.readUE() + 1;
    pps.weighted_pred_flag = br.readFlag();
    pps.weighted_bipred_idc = br.readBits(2);

    pps.pic_init_qp = br.readSE() + 26;
    pps.pic_init_qs = br.readSE() + 26;
    pps.chroma_qp_index_offset = br.readSE();

    pps.deblocking_filter_control_present = br.readFlag();
    pps.constrained_intra_pred_flag = br.readFlag();
    pps.redundant_pic_cnt_present = br.readFlag();

    // More RBSP data (8x8 transform, etc.)
    if (br.moreRbspData()) {
        pps.transform_8x8_mode_flag = br.readFlag();
        pps.pic_scaling_matrix_present = br.readFlag();
        if (pps.pic_scaling_matrix_present) {
            int count = 6 + (pps.transform_8x8_mode_flag ? 2 : 0);
            for (int i = 0; i < count; i++) {
                bool present = br.readFlag();
                if (present) {
                    parseScalingList(br, (i < 6) ? 16 : 64);
                }
            }
        }
        pps.second_chroma_qp_index_offset = br.readSE();
    }

    return true;
}

bool H264Parser::parseSliceHeader(const uint8_t* rbsp, size_t size,
                                   const H264SPS& sps, const H264PPS& pps,
                                   uint8_t nal_unit_type, H264SliceHeader& header) {
    auto clean_rbsp = removeEmulationPrevention(rbsp, size);
    BitstreamReader br(clean_rbsp.data(), clean_rbsp.size());

    header.first_mb_in_slice = br.readUE();
    header.slice_type = br.readUE();
    if (header.slice_type > 9) return false;
    if (header.slice_type > 4) header.slice_type -= 5;  // Map 5-9 to 0-4

    header.pps_id = br.readUE();

    // color_plane_id for separate_colour_plane (skip for now)

    header.frame_num = br.readBits(sps.log2_max_frame_num);

    if (!sps.frame_mbs_only_flag) {
        header.field_pic_flag = br.readFlag();
        if (header.field_pic_flag) {
            header.bottom_field_flag = br.readFlag();
        }
    }

    bool is_idr = (nal_unit_type == 5);
    if (is_idr) {
        header.idr_pic_id = br.readUE();
    }

    if (sps.pic_order_cnt_type == 0) {
        header.pic_order_cnt_lsb = br.readBits(sps.log2_max_pic_order_cnt_lsb);
        if (pps.bottom_field_pic_order_in_frame_present && !header.field_pic_flag) {
            header.delta_pic_order_cnt_bottom = br.readSE();
        }
    }

    if (sps.pic_order_cnt_type == 1 && !sps.delta_pic_order_always_zero_flag) {
        header.delta_pic_order_cnt[0] = br.readSE();
        if (pps.bottom_field_pic_order_in_frame_present && !header.field_pic_flag) {
            header.delta_pic_order_cnt[1] = br.readSE();
        }
    }

    // B-slice direct_spatial_mv_pred
    if (header.slice_type == 1) {  // B-slice
        header.direct_spatial_mv_pred_flag = br.readFlag();
    }

    // Reference picture list override
    if (header.slice_type == 0 || header.slice_type == 1) {  // P or B
        header.num_ref_idx_active_override_flag = br.readFlag();
        if (header.num_ref_idx_active_override_flag) {
            header.num_ref_idx_l0_active = br.readUE() + 1;
            if (header.slice_type == 1) {
                header.num_ref_idx_l1_active = br.readUE() + 1;
            }
        } else {
            header.num_ref_idx_l0_active = pps.num_ref_idx_l0_default_active;
            header.num_ref_idx_l1_active = pps.num_ref_idx_l1_default_active;
        }
    }

    // Reference picture list modification (simplified - skip)
    parseRefPicListModification(br, header.slice_type, header);

    // Prediction weight table (simplified - skip)
    if ((pps.weighted_pred_flag && (header.slice_type == 0 || header.slice_type == 3)) ||
        (pps.weighted_bipred_idc == 1 && header.slice_type == 1)) {
        parsePredWeightTable(br, sps, pps, header);
    }

    // dec_ref_pic_marking
    parseDecRefPicMarking(br, is_idr, header);

    // CABAC init idc
    if (pps.entropy_coding_mode_flag && header.slice_type != 2 && header.slice_type != 4) {
        br.readUE();  // cabac_init_idc
    }

    header.slice_qp_delta = br.readSE();

    return true;
}

void H264Parser::parseScalingList(BitstreamReader& br, int size) {
    int last_scale = 8, next_scale = 8;
    for (int i = 0; i < size; i++) {
        if (next_scale != 0) {
            int delta_scale = br.readSE();
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

void H264Parser::parseHrdParameters(BitstreamReader& br) {
    uint32_t cpb_cnt = br.readUE() + 1;
    br.skipBits(4);  // bit_rate_scale
    br.skipBits(4);  // cpb_size_scale
    for (uint32_t i = 0; i < cpb_cnt; i++) {
        br.readUE();  // bit_rate_value_minus1
        br.readUE();  // cpb_size_value_minus1
        br.readFlag(); // cbr_flag
    }
    br.skipBits(5);  // initial_cpb_removal_delay_length_minus1
    br.skipBits(5);  // cpb_removal_delay_length_minus1
    br.skipBits(5);  // dpb_output_delay_length_minus1
    br.skipBits(5);  // time_offset_length
}

void H264Parser::parseVuiParameters(BitstreamReader& br, H264SPS& sps) {
    bool aspect_ratio_info_present = br.readFlag();
    if (aspect_ratio_info_present) {
        uint8_t aspect_ratio_idc = br.readBits(8);
        if (aspect_ratio_idc == 255) {  // Extended_SAR
            br.skipBits(16);  // sar_width
            br.skipBits(16);  // sar_height
        }
    }

    bool overscan_info_present = br.readFlag();
    if (overscan_info_present) br.readFlag();

    bool video_signal_type_present = br.readFlag();
    if (video_signal_type_present) {
        br.skipBits(3);  // video_format
        br.readFlag();   // video_full_range_flag
        bool colour_description_present = br.readFlag();
        if (colour_description_present) {
            br.skipBits(8);  // colour_primaries
            br.skipBits(8);  // transfer_characteristics
            br.skipBits(8);  // matrix_coefficients
        }
    }

    bool chroma_loc_info_present = br.readFlag();
    if (chroma_loc_info_present) {
        br.readUE();  // chroma_sample_loc_type_top_field
        br.readUE();  // chroma_sample_loc_type_bottom_field
    }

    sps.timing_info_present = br.readFlag();
    if (sps.timing_info_present) {
        sps.num_units_in_tick = br.readBits(32);
        sps.time_scale = br.readBits(32);
        br.readFlag();  // fixed_frame_rate_flag
    }

    bool nal_hrd_parameters_present = br.readFlag();
    if (nal_hrd_parameters_present) parseHrdParameters(br);

    bool vcl_hrd_parameters_present = br.readFlag();
    if (vcl_hrd_parameters_present) parseHrdParameters(br);

    if (nal_hrd_parameters_present || vcl_hrd_parameters_present) {
        br.readFlag();  // low_delay_hrd_flag
    }

    br.readFlag();  // pic_struct_present_flag
    bool bitstream_restriction_flag = br.readFlag();
    if (bitstream_restriction_flag) {
        br.readFlag();  // motion_vectors_over_pic_boundaries_flag
        br.readUE();    // max_bytes_per_pic_denom
        br.readUE();    // max_bits_per_mb_denom
        br.readUE();    // log2_max_mv_length_horizontal
        br.readUE();    // log2_max_mv_length_vertical
        br.readUE();    // max_num_reorder_frames
        br.readUE();    // max_dec_frame_buffering
    }
}

void H264Parser::parseRefPicListModification(BitstreamReader& br, uint8_t slice_type,
                                              H264SliceHeader& header) {
    if (slice_type != 2 && slice_type != 4) {  // Not I or SI
        header.ref_pic_list_modification_flag_l0 = br.readFlag();
        if (header.ref_pic_list_modification_flag_l0) {
            uint32_t modification_of_pic_nums_idc;
            do {
                modification_of_pic_nums_idc = br.readUE();
                if (modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1) {
                    br.readUE();  // abs_diff_pic_num_minus1
                } else if (modification_of_pic_nums_idc == 2) {
                    br.readUE();  // long_term_pic_num
                }
            } while (modification_of_pic_nums_idc != 3);
        }
    }

    if (slice_type == 1) {  // B-slice
        header.ref_pic_list_modification_flag_l1 = br.readFlag();
        if (header.ref_pic_list_modification_flag_l1) {
            uint32_t modification_of_pic_nums_idc;
            do {
                modification_of_pic_nums_idc = br.readUE();
                if (modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1) {
                    br.readUE();
                } else if (modification_of_pic_nums_idc == 2) {
                    br.readUE();
                }
            } while (modification_of_pic_nums_idc != 3);
        }
    }
}

void H264Parser::parsePredWeightTable(BitstreamReader& br, const H264SPS& sps,
                                       const H264PPS& /* pps */, H264SliceHeader& header) {
    br.readUE();  // luma_log2_weight_denom

    int chroma_array_type = (sps.chroma_format_idc == 0) ? 0 : 1;
    if (chroma_array_type != 0) {
        br.readUE();  // chroma_log2_weight_denom
    }

    for (int i = 0; i < header.num_ref_idx_l0_active; i++) {
        bool luma_weight_flag = br.readFlag();
        if (luma_weight_flag) {
            br.readSE();  // luma_weight_l0
            br.readSE();  // luma_offset_l0
        }
        if (chroma_array_type != 0) {
            bool chroma_weight_flag = br.readFlag();
            if (chroma_weight_flag) {
                for (int j = 0; j < 2; j++) {
                    br.readSE();  // chroma_weight_l0
                    br.readSE();  // chroma_offset_l0
                }
            }
        }
    }

    if (header.slice_type == 1) {  // B-slice
        for (int i = 0; i < header.num_ref_idx_l1_active; i++) {
            bool luma_weight_flag = br.readFlag();
            if (luma_weight_flag) {
                br.readSE();
                br.readSE();
            }
            if (chroma_array_type != 0) {
                bool chroma_weight_flag = br.readFlag();
                if (chroma_weight_flag) {
                    for (int j = 0; j < 2; j++) {
                        br.readSE();
                        br.readSE();
                    }
                }
            }
        }
    }
}

void H264Parser::parseDecRefPicMarking(BitstreamReader& br, bool idr,
                                        H264SliceHeader& header) {
    header.mmco_commands.clear();

    if (idr) {
        header.no_output_of_prior_pics_flag = br.readFlag();
        header.long_term_reference_flag = br.readFlag();
    } else {
        header.adaptive_ref_pic_marking_mode_flag = br.readFlag();
        if (header.adaptive_ref_pic_marking_mode_flag) {
            uint32_t memory_management_control_operation;
            do {
                memory_management_control_operation = br.readUE();

                if (memory_management_control_operation != 0) {
                    MMCOCommand cmd;
                    cmd.operation = memory_management_control_operation;

                    // MMCO 1: Mark short-term as "unused for reference"
                    // MMCO 3: Mark short-term as long-term
                    if (memory_management_control_operation == 1 ||
                        memory_management_control_operation == 3) {
                        cmd.difference_of_pic_nums_minus1 = br.readUE();
                    }

                    // MMCO 2: Mark long-term as "unused for reference"
                    if (memory_management_control_operation == 2) {
                        cmd.long_term_pic_num = br.readUE();
                    }

                    // MMCO 3: Mark short-term as long-term (also needs frame idx)
                    // MMCO 6: Mark current as long-term
                    if (memory_management_control_operation == 3 ||
                        memory_management_control_operation == 6) {
                        cmd.long_term_frame_idx = br.readUE();
                    }

                    // MMCO 4: Set max long-term frame index
                    if (memory_management_control_operation == 4) {
                        cmd.max_long_term_frame_idx_plus1 = br.readUE();
                    }

                    header.mmco_commands.push_back(cmd);
                }
            } while (memory_management_control_operation != 0);
        }
    }
}

} // namespace mirage::video
