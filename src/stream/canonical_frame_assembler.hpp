// =============================================================================
// X1 Canonical Frame Assembler  v3
// Stats: dropped_stale → incomplete_frames_evicted (意味明確化)
// =============================================================================
#pragma once
#include "x1_protocol.hpp"
#include "canonical_frame.hpp"
#include "mirage_log.hpp"
#include "stb_image.h"

#include <cstdint>
#include <functional>
#include <map>
#include <vector>
#include <chrono>

namespace mirage::x1 {

class CanonicalFrameAssembler {
public:
    using FrameCallback = std::function<void(CanonicalFrame)>;

    explicit CanonicalFrameAssembler() = default;

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

        // Fast path: single-fragment JPEG
        if (hdr.is_single_frag()) {
            ++stats_.frames_announced;
            stats_.fragments_expected += 1;
            deliver(hdr, payload, payload_len);
            return;
        }

        // Multi-fragment: buffer until complete
        auto& buf = buffers_[hdr.frame_id];
        if (buf.fragments.empty()) {
            // 初回受信: このframe_idをannounce
            ++stats_.frames_announced;
            stats_.fragments_expected += hdr.frag_tot;
            buf.total   = hdr.frag_tot;
            buf.recv_us = now_us();
            buf.pts_us  = hdr.pts_us;
            buf.fragments.resize(hdr.frag_tot);
        }

        if (hdr.frag_idx < buf.fragments.size() &&
            buf.fragments[hdr.frag_idx].empty()) {
            buf.fragments[hdr.frag_idx].assign(payload, payload + payload_len);
            ++buf.received;
        }

        if (buf.received == buf.total) {
            size_t total_bytes = 0;
            for (auto& f : buf.fragments) total_bytes += f.size();

            std::vector<uint8_t> assembled;
            assembled.reserve(total_bytes);
            for (auto& f : buf.fragments)
                assembled.insert(assembled.end(), f.begin(), f.end());

            FrameHeader fhdr = hdr;
            fhdr.pts_us = buf.pts_us;
            deliver(fhdr, assembled.data(), assembled.size());
            buffers_.erase(hdr.frame_id);
        }
    }

    // ── Flush stale incomplete frames ─────────────────────────────────────────
    void flush_stale(uint64_t stale_ms = 150) {
        uint64_t now = now_us();
        auto it = buffers_.begin();
        while (it != buffers_.end()) {
            if ((now - it->second.recv_us) / 1000 > stale_ms) {
                MLOG_WARN("x1_assm", "frame %u stale (%u/%u frags), evicting",
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
        // Delivery
        uint64_t delivered                 = 0;  // frames dispatched to callback
        uint64_t frames_announced          = 0;  // 新しいframe_idを初見した回数
        // Decode failures (drop理由と分離)
        uint64_t frames_decode_failed      = 0;  // stbi_load returned null
        uint64_t decoded_size_mismatch     = 0;  // decoded w/h != CANONICAL_W/H
        // Drops
        uint64_t dropped_old               = 0;  // frame_id < last_delivered (OOO)
        uint64_t incomplete_frames_evicted = 0;  // 未完成のまま期限切れで破棄
        // Fragment accounting
        uint64_t fragments_received        = 0;  // 受信パケット総数 (重複含む)
        uint64_t fragments_expected        = 0;  // 各frame初回受信時のfrag_tot合計
    };
    const Stats& stats() const { return stats_; }

private:
    struct FragBuffer {
        uint16_t total    = 0;
        uint16_t received = 0;
        uint64_t recv_us  = 0;
        uint64_t pts_us   = 0;
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
                 const uint8_t*     jpeg_data,
                 size_t             jpeg_len)
    {
        int w = 0, h = 0, ch = 0;
        uint8_t* raw = stbi_load_from_memory(
            jpeg_data, (int)jpeg_len, &w, &h, &ch, 4);

        if (!raw) {
            MLOG_WARN("x1_assm", "JPEG decode failed frame=%u len=%zu",
                      hdr.frame_id, jpeg_len);
            ++stats_.frames_decode_failed;
            return;
        }

        if (w != CANONICAL_W || h != CANONICAL_H) {
            MLOG_WARN("x1_assm", "size mismatch frame=%u decoded=%dx%d expected=%dx%d",
                      hdr.frame_id, w, h, CANONICAL_W, CANONICAL_H);
            stbi_image_free(raw);
            ++stats_.decoded_size_mismatch;
            return;
        }

        const uint64_t t_decode_done = now_us();
        CanonicalFrame frame;
        frame.frame_id       = hdr.frame_id;
        frame.pts_us         = hdr.pts_us & 0xFFFFFFFFULL;  // 下位32bit = capture_us
        frame.encode_done_us = (hdr.pts_us >> 32) * 1000ULL; // 上位32bit = encode_done_ms → us
        frame.width          = (uint32_t)w;
        frame.height         = (uint32_t)h;
        frame.recv_us        = t_decode_done;
        frame.rgba     = std::shared_ptr<uint8_t[]>(
            raw, [](uint8_t* p){ stbi_image_free(p); });

        last_delivered_id_ = hdr.frame_id;
        ++stats_.delivered;

        if (on_frame_) on_frame_(std::move(frame));
    }
};

} // namespace mirage::x1
