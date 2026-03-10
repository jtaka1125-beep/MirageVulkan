// =============================================================================
// MonitorDecoder — Phase C-4: H.264 software decoder
//
// Pipeline:
//   MonitorFrame (Annex-B NAL bytes)
//     └─ AVCodecContext (H.264 / libavcodec)
//          └─ AVFrame (YUV420P or similar)
//               └─ DecodedFrameCallback(width, height, frame_id, pts_us)
//
// Design notes:
//   - Annex-B input: start codes (00 00 00 01) are passed directly to the parser
//   - SPS/PPS are prepended on keyframes by MonitorEncoder — no separate config needed
//   - Decoder is single-threaded (FFmpeg thread_count=1) to keep latency low
//   - Pixel data is intentionally NOT copied out (no display in C-4) — only stats
//   - Thread-safe: decode() can be called from MonitorReceiver's recv thread
// =============================================================================
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
}

#include "monitor_frame.hpp"
#include "mirage_log.hpp"

#include <cstdint>
#include <functional>
#include <atomic>
#include <mutex>
#include <chrono>
#include <string>

namespace mirage::x1 {

class MonitorDecoder {
public:
    struct DecodedFrame {
        uint32_t frame_id    = 0;
        uint64_t pts_us      = 0;
        uint64_t decode_us   = 0;   // PC-side decode-done timestamp
        int      width       = 0;
        int      height      = 0;
        int      format      = 0;   // AVPixelFormat
        bool     is_keyframe = false;
    };

    using FrameCallback = std::function<void(DecodedFrame)>;

    MonitorDecoder() = default;
    ~MonitorDecoder() { close(); }

    // Non-copyable
    MonitorDecoder(const MonitorDecoder&)            = delete;
    MonitorDecoder& operator=(const MonitorDecoder&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    bool open() {
        std::lock_guard<std::mutex> lk(ctx_mutex_);
        if (codec_ctx_) return true;  // already open

        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            MLOG_ERROR("monitor_dec", "H.264 decoder not found");
            return false;
        }

        parser_ = av_parser_init(AV_CODEC_ID_H264);
        if (!parser_) {
            MLOG_ERROR("monitor_dec", "av_parser_init failed");
            return false;
        }

        codec_ctx_ = avcodec_alloc_context3(codec);
        if (!codec_ctx_) {
            MLOG_ERROR("monitor_dec", "avcodec_alloc_context3 failed");
            av_parser_close(parser_); parser_ = nullptr;
            return false;
        }

        // Low-latency: 1 thread, no delay
        codec_ctx_->thread_count = 1;
        codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;

        int ret = avcodec_open2(codec_ctx_, codec, nullptr);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            MLOG_ERROR("monitor_dec", "avcodec_open2 failed: %s", errbuf);
            avcodec_free_context(&codec_ctx_);
            av_parser_close(parser_); parser_ = nullptr;
            return false;
        }

        pkt_   = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!pkt_ || !frame_) {
            MLOG_ERROR("monitor_dec", "alloc failed");
            close_locked();
            return false;
        }

        MLOG_INFO("monitor_dec", "opened H.264 decoder (sw, thread_count=1)");
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lk(ctx_mutex_);
        close_locked();
    }

    void set_callback(FrameCallback cb) {
        std::lock_guard<std::mutex> lk(ctx_mutex_);
        on_frame_ = std::move(cb);
    }

    // ── Decode one MonitorFrame ────────────────────────────────────────────
    // Thread-safe. Returns number of decoded frames (0 on no-output, <0 on error).

    int decode(const MonitorFrame& mf) {
        if (!mf.nal_data || mf.nal_data->empty()) return 0;

        std::lock_guard<std::mutex> lk(ctx_mutex_);
        if (!codec_ctx_ || !parser_ || !pkt_ || !frame_) return -1;

        const uint8_t* data     = mf.nal_data->data();
        int            data_len = (int)mf.nal_data->size();
        int            decoded  = 0;

        // Feed data through Annex-B parser — handles split/merged NAL units
        while (data_len > 0) {
            int parsed = av_parser_parse2(
                parser_, codec_ctx_,
                &pkt_->data, &pkt_->size,
                data, data_len,
                (int64_t)mf.pts_us,   // pts
                AV_NOPTS_VALUE,        // dts
                0);
            if (parsed < 0) {
                ++stats_.parse_errors;
                break;
            }
            data     += parsed;
            data_len -= parsed;

            if (pkt_->size > 0) {
                int ret = avcodec_send_packet(codec_ctx_, pkt_);
                if (ret < 0) {
                    if (ret != AVERROR(EAGAIN)) {
                        ++stats_.send_errors;
                    }
                } else {
                    decoded += drain_frames(mf.frame_id, mf.is_keyframe);
                }
            }
        }

        ++stats_.nals_fed;
        return decoded;
    }

    // ── Stats ─────────────────────────────────────────────────────────────

    struct Stats {
        uint64_t nals_fed         = 0;
        uint64_t frames_decoded   = 0;
        uint64_t keyframes_decoded = 0;
        uint64_t send_errors      = 0;
        uint64_t recv_errors      = 0;
        uint64_t parse_errors     = 0;
        int      last_width       = 0;
        int      last_height      = 0;
        int      last_format      = 0;
    };

    Stats get_stats() const {
        std::lock_guard<std::mutex> lk(ctx_mutex_);
        return stats_;
    }

    void log_stats(const char* prefix = "decoder") const {
        Stats s = get_stats();
        MLOG_INFO("monitor_dec",
            "%s: decoded=%llu keys=%llu parse_err=%llu send_err=%llu "
            "recv_err=%llu res=%dx%d",
            prefix,
            (unsigned long long)s.frames_decoded,
            (unsigned long long)s.keyframes_decoded,
            (unsigned long long)s.parse_errors,
            (unsigned long long)s.send_errors,
            (unsigned long long)s.recv_errors,
            s.last_width, s.last_height);
    }

private:
    mutable std::mutex  ctx_mutex_;
    const AVCodec*      codec_     = nullptr;
    AVCodecParserContext* parser_  = nullptr;
    AVCodecContext*     codec_ctx_ = nullptr;
    AVPacket*           pkt_       = nullptr;
    AVFrame*            frame_     = nullptr;
    FrameCallback       on_frame_;
    Stats               stats_;

    int drain_frames(uint32_t frame_id, bool is_key) {
        int count = 0;
        while (true) {
            int ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { ++stats_.recv_errors; break; }

            ++stats_.frames_decoded;
            if (is_key) ++stats_.keyframes_decoded;
            stats_.last_width  = frame_->width;
            stats_.last_height = frame_->height;
            stats_.last_format = frame_->format;
            ++count;

            if (on_frame_) {
                DecodedFrame df;
                df.frame_id    = frame_id;
                df.pts_us      = (frame_->pts != AV_NOPTS_VALUE)
                                   ? (uint64_t)frame_->pts : 0;
                df.decode_us   = (uint64_t)std::chrono::duration_cast<
                    std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                df.width       = frame_->width;
                df.height      = frame_->height;
                df.format      = frame_->format;
                df.is_keyframe = is_key;
                on_frame_(df);
            }
            av_frame_unref(frame_);
        }
        return count;
    }

    void close_locked() {
        if (frame_)     { av_frame_free(&frame_);         frame_     = nullptr; }
        if (pkt_)       { av_packet_free(&pkt_);           pkt_       = nullptr; }
        if (codec_ctx_) { avcodec_free_context(&codec_ctx_); codec_ctx_ = nullptr; }
        if (parser_)    { av_parser_close(parser_);        parser_    = nullptr; }
    }
};

} // namespace mirage::x1
