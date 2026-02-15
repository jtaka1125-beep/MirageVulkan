// =============================================================================
// MirageSystem - H.264 Bitstream Parser
// =============================================================================
// NAL unit parsing, SPS/PPS extraction, slice header parsing
// Based on ITU-T H.264 (ISO/IEC 14496-10) specification
// =============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>

namespace mirage::video {

// =============================================================================
// H.264 Parameter Set Structures
// =============================================================================
struct H264SPS {
    uint8_t sps_id = 0;
    uint8_t profile_idc = 0;
    uint8_t level_idc = 0;
    uint8_t chroma_format_idc = 1;
    uint8_t bit_depth_luma = 8;
    uint8_t bit_depth_chroma = 8;

    uint32_t pic_width_in_mbs = 0;
    uint32_t pic_height_in_map_units = 0;
    bool frame_mbs_only_flag = true;
    bool direct_8x8_inference_flag = false;

    bool frame_cropping_flag = false;
    uint32_t frame_crop_left = 0;
    uint32_t frame_crop_right = 0;
    uint32_t frame_crop_top = 0;
    uint32_t frame_crop_bottom = 0;

    uint8_t log2_max_frame_num = 4;
    uint8_t pic_order_cnt_type = 0;
    uint8_t log2_max_pic_order_cnt_lsb = 4;
    bool delta_pic_order_always_zero_flag = false;
    int32_t offset_for_non_ref_pic = 0;
    int32_t offset_for_top_to_bottom_field = 0;
    uint8_t num_ref_frames_in_pic_order_cnt_cycle = 0;
    std::vector<int32_t> offset_for_ref_frame;

    uint8_t max_num_ref_frames = 1;
    bool gaps_in_frame_num_allowed = false;

    // VUI parameters (partial)
    bool vui_parameters_present = false;
    bool timing_info_present = false;
    uint32_t num_units_in_tick = 0;
    uint32_t time_scale = 0;

    uint32_t width() const {
        uint32_t w = pic_width_in_mbs * 16;
        if (frame_cropping_flag) {
            uint32_t crop_unit_x = (chroma_format_idc == 0) ? 1 : 2;
            w -= (frame_crop_left + frame_crop_right) * crop_unit_x;
        }
        return w;
    }

    uint32_t height() const {
        uint32_t h = pic_height_in_map_units * 16;
        if (!frame_mbs_only_flag) h *= 2;
        if (frame_cropping_flag) {
            uint32_t crop_unit_y = (chroma_format_idc == 0) ? 1 : 2;
            if (!frame_mbs_only_flag) crop_unit_y *= 2;
            h -= (frame_crop_top + frame_crop_bottom) * crop_unit_y;
        }
        return h;
    }
};

struct H264PPS {
    uint8_t pps_id = 0;
    uint8_t sps_id = 0;

    bool entropy_coding_mode_flag = false;  // 0=CAVLC, 1=CABAC
    bool bottom_field_pic_order_in_frame_present = false;

    uint8_t num_slice_groups = 1;
    uint8_t num_ref_idx_l0_default_active = 1;
    uint8_t num_ref_idx_l1_default_active = 1;

    bool weighted_pred_flag = false;
    uint8_t weighted_bipred_idc = 0;

    int8_t pic_init_qp = 26;
    int8_t pic_init_qs = 26;
    int8_t chroma_qp_index_offset = 0;

    bool deblocking_filter_control_present = false;
    bool constrained_intra_pred_flag = false;
    bool redundant_pic_cnt_present = false;

    bool transform_8x8_mode_flag = false;
    bool pic_scaling_matrix_present = false;
    int8_t second_chroma_qp_index_offset = 0;
};

// MMCO (Memory Management Control Operation) command
struct MMCOCommand {
    uint32_t operation = 0;         // memory_management_control_operation
    uint32_t difference_of_pic_nums_minus1 = 0;   // For ops 1, 3
    uint32_t long_term_pic_num = 0;               // For op 2
    uint32_t long_term_frame_idx = 0;             // For ops 3, 6
    uint32_t max_long_term_frame_idx_plus1 = 0;   // For op 4
};

struct H264SliceHeader {
    uint32_t first_mb_in_slice = 0;
    uint8_t slice_type = 0;         // 0=P, 1=B, 2=I, 3=SP, 4=SI
    uint8_t pps_id = 0;

    uint16_t frame_num = 0;
    bool field_pic_flag = false;
    bool bottom_field_flag = false;

    uint16_t idr_pic_id = 0;
    uint16_t pic_order_cnt_lsb = 0;
    int32_t delta_pic_order_cnt_bottom = 0;
    int32_t delta_pic_order_cnt[2] = {0, 0};

    bool direct_spatial_mv_pred_flag = false;
    bool num_ref_idx_active_override_flag = false;
    uint8_t num_ref_idx_l0_active = 0;
    uint8_t num_ref_idx_l1_active = 0;

    // Reference picture list modification
    bool ref_pic_list_modification_flag_l0 = false;
    bool ref_pic_list_modification_flag_l1 = false;

    // dec_ref_pic_marking
    bool no_output_of_prior_pics_flag = false;
    bool long_term_reference_flag = false;
    bool adaptive_ref_pic_marking_mode_flag = false;

    // MMCO commands (for adaptive_ref_pic_marking_mode_flag == true)
    std::vector<MMCOCommand> mmco_commands;

    int8_t slice_qp_delta = 0;

    bool is_idr() const { return slice_type == 2 || slice_type == 7; }
    bool is_reference() const { return slice_type != 0 && slice_type != 5; }
};

// =============================================================================
// Exponential-Golomb Bitstream Reader
// =============================================================================
class BitstreamReader {
public:
    BitstreamReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), byte_pos_(0), bit_pos_(0) {}

    // Read unsigned Exp-Golomb coded value
    uint32_t readUE() {
        int leading_zeros = 0;
        while (readBit() == 0 && leading_zeros < 32) {
            leading_zeros++;
        }
        if (leading_zeros == 0) return 0;
        uint32_t value = (1u << leading_zeros) - 1 + readBits(leading_zeros);
        return value;
    }

    // Read signed Exp-Golomb coded value
    int32_t readSE() {
        uint32_t ue = readUE();
        int32_t se = (ue + 1) / 2;
        if ((ue & 1) == 0) se = -se;
        return se;
    }

    // Read N bits
    uint32_t readBits(int n) {
        uint32_t value = 0;
        for (int i = 0; i < n; i++) {
            value = (value << 1) | readBit();
        }
        return value;
    }

    // Read single bit
    uint32_t readBit() {
        if (byte_pos_ >= size_) return 0;
        uint32_t bit = (data_[byte_pos_] >> (7 - bit_pos_)) & 1;
        if (++bit_pos_ == 8) {
            bit_pos_ = 0;
            byte_pos_++;
        }
        return bit;
    }

    // Read flag (1 bit as bool)
    bool readFlag() { return readBit() != 0; }

    // Skip bits
    void skipBits(int n) {
        for (int i = 0; i < n; i++) readBit();
    }

    // Check if more data available
    bool hasMoreData() const {
        return byte_pos_ < size_;
    }

    // Check for RBSP trailing bits
    bool moreRbspData() const {
        if (byte_pos_ >= size_) return false;
        // Check if remaining bits are all zeros except for stop bit
        size_t remaining_bytes = size_ - byte_pos_;
        if (remaining_bytes > 1) return true;
        if (remaining_bytes == 1) {
            uint8_t remaining = data_[byte_pos_] & ((1 << (8 - bit_pos_)) - 1);
            return remaining != (1 << (7 - bit_pos_));
        }
        return false;
    }

    size_t bytesRead() const { return byte_pos_; }
    size_t bitsRead() const { return byte_pos_ * 8 + bit_pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t byte_pos_;
    int bit_pos_;
};

// =============================================================================
// NAL Unit Structure
// =============================================================================
struct NalUnit {
    uint8_t nal_ref_idc = 0;        // 2 bits
    uint8_t nal_unit_type = 0;      // 5 bits

    const uint8_t* rbsp_data = nullptr;
    size_t rbsp_size = 0;

    // Original NAL data (with start code)
    const uint8_t* data = nullptr;
    size_t size = 0;

    bool isIDR() const { return nal_unit_type == 5; }
    bool isSPS() const { return nal_unit_type == 7; }
    bool isPPS() const { return nal_unit_type == 8; }
    bool isSlice() const { return nal_unit_type >= 1 && nal_unit_type <= 5; }
    bool isReference() const { return nal_ref_idc != 0; }
};

// =============================================================================
// H.264 Parser
// =============================================================================
class H264Parser {
public:
    H264Parser() = default;

    // Parse NAL units from Annex-B stream (with start codes)
    std::vector<NalUnit> parseAnnexB(const uint8_t* data, size_t size);

    // Parse SPS from RBSP data
    bool parseSPS(const uint8_t* rbsp, size_t size, H264SPS& sps);

    // Parse PPS from RBSP data
    bool parsePPS(const uint8_t* rbsp, size_t size, H264PPS& pps);

    // Parse slice header from RBSP data
    bool parseSliceHeader(const uint8_t* rbsp, size_t size,
                          const H264SPS& sps, const H264PPS& pps,
                          uint8_t nal_unit_type, H264SliceHeader& header);

    // Remove emulation prevention bytes (0x03 after 0x0000)
    static std::vector<uint8_t> removeEmulationPrevention(const uint8_t* data, size_t size);

private:
    // Parse scaling list
    void parseScalingList(BitstreamReader& br, int size);

    // Parse HRD parameters
    void parseHrdParameters(BitstreamReader& br);

    // Parse VUI parameters
    void parseVuiParameters(BitstreamReader& br, H264SPS& sps);

    // Parse reference picture list modification
    void parseRefPicListModification(BitstreamReader& br, uint8_t slice_type,
                                     H264SliceHeader& header);

    // Parse prediction weight table
    void parsePredWeightTable(BitstreamReader& br, const H264SPS& sps,
                              const H264PPS& pps, H264SliceHeader& header);

    // Parse dec_ref_pic_marking
    void parseDecRefPicMarking(BitstreamReader& br, bool idr, H264SliceHeader& header);
};

} // namespace mirage::video
