// =============================================================================
// MonitorFrame — Phase C Monitor Lane frame data
//
// Carries a single complete H.264 NAL unit (or SPS+PPS+IDR bundle on keyframes).
// Raw Annex-B bytes are preserved as-is for downstream decoder.
// =============================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace mirage::x1 {

struct MonitorFrame {
    uint32_t frame_id   = 0;
    uint64_t pts_us     = 0;      // capture timestamp (microseconds)
    uint64_t recv_us    = 0;      // PC-side receive timestamp
    bool     is_keyframe = false;

    // Raw H.264 NAL data (Annex-B, may include SPS+PPS prefix on keyframes)
    std::shared_ptr<std::vector<uint8_t>> nal_data;

    size_t size() const {
        return nal_data ? nal_data->size() : 0;
    }

    bool empty() const { return !nal_data || nal_data->empty(); }
};

} // namespace mirage::x1
