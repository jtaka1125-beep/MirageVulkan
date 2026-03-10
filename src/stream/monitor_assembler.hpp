// =============================================================================
// MonitorAssembler — Phase C: H.264 NAL fragment reassembly
//
// Mirror of CanonicalFrameAssembler but for H.264 NAL data instead of JPEG.
// Delivers MonitorFrame (raw NAL bytes) via callback — no decode here.
//
// Protocol: same StreamProtocol header as Canonical lane, CODEC_H264.
// =============================================================================
#pragma once
#include "x1_protocol.hpp"
#include "monitor_frame.hpp"
#include "mirage_log.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <vector>
#include <chrono>
#include <atomic>

namespace mirage::x1 {

class MonitorAssembler {
public:
    using FrameCallback = std::function<void(MonitorFrame)>;

    explicit MonitorAssembler() = default;

    void set_callback(FrameCallback cb) { on_frame_ = std::move(cb); }

    // ── Feed a single UDP datagram ────────────────────────────────────────────
    void feed(const FrameHeader& hdr,
              const uint8_t*     payload,
              size_t             payload_len)
    {
        ++stats_.fragments_received;

        if (hdr.frame_id < last_delivered_id_) {
            // Detect sender restart: large gap means frame_id wrapped to 0
            uint32_t gap = last_delivered_id_ - hdr.frame_id;
            if (gap > 1000) {
                // Sender restarted: reset last_delivered_id_
                MLOG_INFO("assembler", "[RESTART] frame_id reset: was=%u now=%u, clearing state",
                          last_delivered_id_, hdr.frame_id);
                last_delivered_id_ = 0;
                buffers_.clear();
                // fall through to process this frame normally
            } else {
            ++stats_.dropped_old;
            return;
            }
        }

        // Fast path: single-fragment NAL (small NAL, fits in one datagram)
        if (hdr.is_single_frag()) {
            ++stats_.frames_announced;
            stats_.fragments_expected += 1;
            deliver(hdr, payload, payload_len);
            return;
        }

        // Multi-fragment: buffer until all fragments received
        auto& buf = buffers_[hdr.frame_id];
        if (buf.fragments.empty()) {
            ++stats_.frames_announced;
            stats_.fragments_expected += hdr.frag_tot;
            buf.total    = hdr.frag_tot;
            buf.recv_us  = now_us();
            buf.pts_us   = hdr.pts_us;
            buf.is_key   = (hdr.flags & FLAG_KEYFRAME) != 0;
            buf.fragments.resize(hdr.frag_tot);
        }

        if (hdr.frag_idx < buf.fragments.size() &&
            buf.fragments[hdr.frag_idx].empty()) {
            buf.fragments[hdr.frag_idx].assign(payload, payload + payload_len);
            ++buf.received;
        }

        if (buf.received == buf.total) {
            // All fragments received: reassemble
            size_t total_bytes = 0;
            for (auto& f : buf.fragments) total_bytes += f.size();

            auto assembled = std::make_shared<std::vector<uint8_t>>();
            assembled->reserve(total_bytes);
            for (auto& f : buf.fragments)
                assembled->insert(assembled->end(), f.begin(), f.end());

            FrameHeader fhdr = hdr;
            fhdr.pts_us = buf.pts_us;
            if (buf.is_key) fhdr.flags |= FLAG_KEYFRAME;
            deliver_assembled(fhdr, std::move(assembled));
            buffers_.erase(hdr.frame_id);
        }
    }

    // ── Evict stale incomplete frames (call periodically) ────────────────────
    void flush_stale(uint64_t stale_ms = 200) {
        uint64_t now = now_us();
        auto it = buffers_.begin();
        while (it != buffers_.end()) {
            if ((now - it->second.recv_us) / 1000 > stale_ms) {
                MLOG_WARN("monitor_assm", "frame %u stale (%u/%u frags), evicting",
                          it->first, it->second.received, it->second.total);
                ++stats_.incomplete_frames_evicted;
                it = buffers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ── Stats ─────────────────────────────────────────────────────────────────
    struct Stats {
        uint64_t delivered                 = 0;
        uint64_t frames_announced          = 0;
        uint64_t keyframes_delivered       = 0;
        uint64_t dropped_old               = 0;
        uint64_t incomplete_frames_evicted = 0;
        uint64_t fragments_received        = 0;
        uint64_t fragments_expected        = 0;
        uint64_t bytes_delivered           = 0;
    };
    const Stats& stats() const { return stats_; }

    // Print a one-line stats summary
    void log_stats(const char* prefix = "monitor") const {
        MLOG_INFO("monitor_assm",
            "%s: delivered=%llu keys=%llu dropped_old=%llu evicted=%llu "
            "frags_recv=%llu bytes=%llu KB",
            prefix,
            (unsigned long long)stats_.delivered,
            (unsigned long long)stats_.keyframes_delivered,
            (unsigned long long)stats_.dropped_old,
            (unsigned long long)stats_.incomplete_frames_evicted,
            (unsigned long long)stats_.fragments_received,
            (unsigned long long)(stats_.bytes_delivered / 1024));
    }

private:
    static constexpr uint8_t FLAG_KEYFRAME = 0x01;

    struct FragBuffer {
        uint16_t total    = 0;
        uint16_t received = 0;
        uint64_t recv_us  = 0;
        uint64_t pts_us   = 0;
        bool     is_key   = false;
        std::vector<std::vector<uint8_t>> fragments;
    };

    FrameCallback  on_frame_;
    uint32_t       last_delivered_id_ = 0;
    std::map<uint32_t, FragBuffer> buffers_;
    Stats          stats_;

    static uint64_t now_us() {
        using namespace std::chrono;
        return (uint64_t)duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    void deliver(const FrameHeader& hdr,
                 const uint8_t*     data,
                 size_t             len)
    {
        auto nal = std::make_shared<std::vector<uint8_t>>(data, data + len);
        deliver_assembled(hdr, std::move(nal));
    }

    void deliver_assembled(const FrameHeader&                      hdr,
                           std::shared_ptr<std::vector<uint8_t>>  nal)
    {
        last_delivered_id_ = hdr.frame_id;
        ++stats_.delivered;
        stats_.bytes_delivered += nal->size();

        MonitorFrame frame;
        frame.frame_id    = hdr.frame_id;
        frame.pts_us      = hdr.pts_us;
        frame.recv_us     = now_us();
        frame.is_keyframe = (hdr.flags & FLAG_KEYFRAME) != 0;
        frame.nal_data    = std::move(nal);

        if (frame.is_keyframe) ++stats_.keyframes_delivered;

        if (on_frame_) on_frame_(std::move(frame));
    }
};

} // namespace mirage::x1
