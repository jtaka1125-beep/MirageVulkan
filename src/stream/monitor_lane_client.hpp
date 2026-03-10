// =============================================================================
// MonitorLaneClient — Phase C-5: Monitor Lane receive + decode → RGBA callback
//
// Self-contained pipeline:
//   UDP :50202 → MonitorReceiver → MonitorAssembler
//     → MonitorFrame (Annex-B NAL)
//       → H264Decoder (libavcodec SW, sws_scale YUV→RGBA)
//         → RgbaCallback(device_id, rgba_ptr, w, h, pts_us)
//
// Diagnostic logs (MLOG_INFO "monitor_client"):
//   [START]         started device=X port=50202
//   [RECV_FIRST]    first NAL received  (is_keyframe, frame_id)
//   [KEYFRAME]      first keyframe NAL received
//   [DECODE_FIRST]  first RGBA callback  w×h
//   [STAGE_FIRST]   first stageMonitorFrame call (via GUI callback)
//   [PERIODIC]      every 5 s: recv=N  decoded=N  rgba=N  w×h  age_ms
//   [STOP]          stopped
// =============================================================================
#pragma once

#include "stream/monitor_receiver.hpp"
#include "stream/monitor_frame.hpp"
#include "video/h264_decoder.hpp"
#include "mirage_log.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>

namespace mirage::x1 {

class MonitorLaneClient {
public:
    // Called with decoded RGBA frame. rgba is valid only during the call.
    using RgbaCallback = std::function<void(
        const std::string& device_id,
        const uint8_t*     rgba,
        int                width,
        int                height,
        uint64_t           pts_us)>;

    MonitorLaneClient() = default;
    ~MonitorLaneClient() { stop(); }

    MonitorLaneClient(const MonitorLaneClient&)            = delete;
    MonitorLaneClient& operator=(const MonitorLaneClient&) = delete;

    void set_device_id(const std::string& id) { device_id_ = id; }

    void set_callback(RgbaCallback cb) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        on_rgba_ = std::move(cb);
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    bool start(uint16_t udp_port = 50202) {
        if (running_.load()) return true;

        // Reset diagnostic counters
        frames_recv_    = 0;
        frames_decoded_ = 0;
        rgba_calls_     = 0;
        first_recv_logged_    = false;
        first_keyframe_logged_= false;
        first_decode_logged_  = false;
        last_periodic_sec_    = 0;
        last_rgba_time_ms_    = 0;
        last_w_ = last_h_     = 0;

        // Build decoder
        decoder_ = std::make_unique<::gui::H264Decoder>();
        if (!decoder_->init()) {
            MLOG_ERROR("monitor_client", "[ERROR] H264Decoder::init() failed");
            decoder_.reset();
            return false;
        }

        decoder_->set_frame_callback([this](const uint8_t* rgba, int w, int h, uint64_t pts) {
            uint64_t n = ++frames_decoded_;
            last_w_ = w;
            last_h_ = h;
            last_rgba_time_ms_ = now_ms();

            if (!first_decode_logged_) {
                first_decode_logged_ = true;
                MLOG_INFO("monitor_client",
                    "[DECODE_FIRST] first RGBA callback  #%llu  %dx%d  pts=%llu",
                    (unsigned long long)n, w, h, (unsigned long long)pts);
            }

            // Periodic log every 5 s
            uint64_t sec5 = last_rgba_time_ms_ / 5000;
            if (sec5 != last_periodic_sec_) {
                last_periodic_sec_ = sec5;
                MLOG_INFO("monitor_client",
                    "[PERIODIC] recv=%llu decoded=%llu rgba=%llu  %dx%d  age_ms=0",
                    (unsigned long long)frames_recv_.load(),
                    (unsigned long long)frames_decoded_.load(),
                    (unsigned long long)rgba_calls_.load(),
                    w, h);
            }

            ++rgba_calls_;
            std::lock_guard<std::mutex> lk(cb_mutex_);
            if (on_rgba_) on_rgba_(device_id_, rgba, w, h, pts);
        });

        // Build receiver
        receiver_ = std::make_unique<MonitorReceiver>();
        receiver_->set_callback([this](MonitorFrame f) {
            uint64_t n = ++frames_recv_;

            if (!first_recv_logged_) {
                first_recv_logged_ = true;
                MLOG_INFO("monitor_client",
                    "[RECV_FIRST] first NAL  #%llu  keyframe=%d  frame_id=%u  bytes=%zu",
                    (unsigned long long)n,
                    (int)f.is_keyframe,
                    f.frame_id,
                    f.nal_data ? f.nal_data->size() : 0u);
            }

            if (f.is_keyframe && !first_keyframe_logged_) {
                first_keyframe_logged_ = true;
                MLOG_INFO("monitor_client",
                    "[KEYFRAME] first keyframe NAL  #%llu  frame_id=%u",
                    (unsigned long long)n, f.frame_id);
            }

            if (!f.nal_data || f.nal_data->empty()) return;
            decoder_->decode(f.nal_data->data(), f.nal_data->size());
        });

        if (!receiver_->start(udp_port)) {
            MLOG_ERROR("monitor_client", "[ERROR] MonitorReceiver::start(%d) failed", (int)udp_port);
            decoder_.reset();
            receiver_.reset();
            return false;
        }

        running_.store(true);
        MLOG_INFO("monitor_client", "[START] started  device=%s  port=%d",
                  device_id_.c_str(), (int)udp_port);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        MLOG_INFO("monitor_client",
            "[STOP] stopped  device=%s  recv=%llu decoded=%llu rgba=%llu",
            device_id_.c_str(),
            (unsigned long long)frames_recv_.load(),
            (unsigned long long)frames_decoded_.load(),
            (unsigned long long)rgba_calls_.load());
        if (receiver_) { receiver_->stop(); receiver_.reset(); }
        decoder_.reset();
    }

    bool is_running() const { return running_.load(); }

    // ── Stats ──────────────────────────────────────────────────────────────
    uint64_t frames_recv()    const { return frames_recv_.load(); }
    uint64_t frames_decoded() const { return frames_decoded_.load(); }
    uint64_t rgba_calls()     const { return rgba_calls_.load(); }
    int      last_width()     const { return last_w_; }
    int      last_height()    const { return last_h_; }
    // ms since last RGBA callback (0 = never received)
    uint64_t last_rgba_age_ms() const {
        uint64_t t = last_rgba_time_ms_;
        return t ? (now_ms() - t) : 0;
    }

private:
    static uint64_t now_ms() {
        using namespace std::chrono;
        return (uint64_t)duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
    }

    std::string   device_id_;
    std::mutex    cb_mutex_;
    RgbaCallback  on_rgba_;

    std::unique_ptr<MonitorReceiver>    receiver_;
    std::unique_ptr<::gui::H264Decoder> decoder_;

    std::atomic<bool>     running_       {false};
    std::atomic<uint64_t> frames_recv_   {0};
    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> rgba_calls_    {0};

    // Diagnostics (written only from recv/decode threads — single writer per flag)
    std::atomic<bool>     first_recv_logged_    {false};
    std::atomic<bool>     first_keyframe_logged_{false};
    std::atomic<bool>     first_decode_logged_  {false};
    std::atomic<uint64_t> last_periodic_sec_    {0};
    std::atomic<uint64_t> last_rgba_time_ms_    {0};
    std::atomic<int>      last_w_               {0};
    std::atomic<int>      last_h_               {0};
};

} // namespace mirage::x1
