#include "hybrid_receiver.hpp"
#include <cstdio>
#include "mirage_log.hpp"

namespace gui {

HybridReceiver::HybridReceiver() {
    bandwidth_state_.last_update = std::chrono::steady_clock::now();
    bandwidth_state_.last_usb_packet = bandwidth_state_.last_update;
    bandwidth_state_.last_wifi_packet = bandwidth_state_.last_update;
    bandwidth_state_.last_frame = bandwidth_state_.last_update;
}

HybridReceiver::~HybridReceiver() {
    stop();
}

bool HybridReceiver::start(uint16_t wifi_port) {
    if (running_) return true;

    MLOG_INFO("hybrid", "Starting hybrid receiver (WiFi port: %d)", wifi_port);

    // Start WiFi receiver (always runs - does the decoding)
    wifi_receiver_ = std::make_unique<MirrorReceiver>();
    if (!wifi_receiver_->start(wifi_port)) {
        MLOG_ERROR("hybrid", "Failed to start WiFi receiver");
    }

    // USB video is now handled by MultiUsbCommandSender and fed via feed_usb_data()
    // Don't start UsbVideoReceiver here - it would conflict with MultiUsbCommandSender
    MLOG_INFO("hybrid", "USB video will be fed via MultiUsbCommandSender");

    // Set initial source
    if (active_source_ == Source::None) {
        if (wifi_receiver_) {
            active_source_ = Source::WiFi;
        }
    }

    if (!wifi_receiver_) {
        return false;
    }

    running_ = true;

    // Initialize bandwidth state
    {
        std::lock_guard<std::mutex> lock(stats_mtx_);
        bandwidth_state_.last_update = std::chrono::steady_clock::now();
        bandwidth_state_.last_switch_reason = "Initial";
    }

    MLOG_INFO("hybrid", "Active source: %s", active_source_name());
    return true;
}

void HybridReceiver::stop() {
    running_ = false;

    if (usb_receiver_) {
        usb_receiver_->stop();
        usb_receiver_.reset();
    }

    if (wifi_receiver_) {
        wifi_receiver_->stop();
        wifi_receiver_.reset();
    }

    active_source_ = Source::None;
}

void HybridReceiver::updateBandwidthStats() {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(stats_mtx_);

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - bandwidth_state_.last_update
    ).count();

    // Update every 100ms
    if (elapsed_ms < 100) return;

    float elapsed_sec = elapsed_ms / 1000.0f;

    // Get current values
    uint64_t usb_pkts = usb_receiver_ ? usb_receiver_->packets_received() : 0;
    uint64_t usb_bytes = usb_receiver_ ? usb_receiver_->bytes_received() : 0;
    uint64_t wifi_pkts = wifi_receiver_ ? wifi_receiver_->packets_received() : 0;
    uint64_t wifi_bytes = wifi_receiver_ ? wifi_receiver_->bytes_received() : 0;
    uint64_t frames = wifi_receiver_ ? wifi_receiver_->frames_decoded() : 0;

    // Calculate rates
    bandwidth_state_.usb_packet_rate = (usb_pkts - bandwidth_state_.prev_usb_packets) / elapsed_sec;
    bandwidth_state_.usb_bandwidth_mbps = ((usb_bytes - bandwidth_state_.prev_usb_bytes) * 8.0f) / (elapsed_sec * 1000000.0f);
    bandwidth_state_.wifi_packet_rate = (wifi_pkts - bandwidth_state_.prev_wifi_packets) / elapsed_sec;
    bandwidth_state_.wifi_bandwidth_mbps = ((wifi_bytes - bandwidth_state_.prev_wifi_bytes) * 8.0f) / (elapsed_sec * 1000000.0f);
    bandwidth_state_.decode_fps = (frames - bandwidth_state_.prev_frames) / elapsed_sec;

    // USB latency estimation (time since last USB packet)
    uint64_t last_usb_ms = usb_last_packet_time_.load();
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    bandwidth_state_.usb_latency_ms = (last_usb_ms > 0) ? (now_ms - last_usb_ms) : 999.0f;

    // Store for next calculation
    bandwidth_state_.prev_usb_packets = usb_pkts;
    bandwidth_state_.prev_usb_bytes = usb_bytes;
    bandwidth_state_.prev_wifi_packets = wifi_pkts;
    bandwidth_state_.prev_wifi_bytes = wifi_bytes;
    bandwidth_state_.prev_frames = frames;
    bandwidth_state_.last_update = now;
}

void HybridReceiver::evaluateSourceSwitch() {
    std::lock_guard<std::mutex> lock(stats_mtx_);

    // Anti-flapping: Check cooldown period
    auto now = std::chrono::steady_clock::now();
    auto cooldown_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - bandwidth_state_.last_switch_time
    ).count();

    bandwidth_state_.in_cooldown = (cooldown_elapsed < config_.switch_cooldown_ms);
    if (bandwidth_state_.in_cooldown && active_source_ != Source::None) {
        // During cooldown, don't switch (except from None state)
        return;
    }

    uint64_t now_ms_eval = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    uint64_t last_usb_ms_eval = usb_last_packet_time_.load();
    bool usb_available = (last_usb_ms_eval > 0) && ((now_ms_eval - last_usb_ms_eval) < 500ULL);
    bool wifi_available = wifi_receiver_ && bandwidth_state_.wifi_packet_rate > 0;

    // Detect USB congestion
    bool usb_congested_now = false;
    if (usb_available) {
        if (bandwidth_state_.usb_latency_ms > config_.usb_max_latency_ms) {
            usb_congested_now = true;
        }
        if (bandwidth_state_.usb_packet_rate < config_.usb_min_packet_rate &&
            bandwidth_state_.prev_usb_packets > 0) {
            usb_congested_now = true;
        }
        if (bandwidth_state_.usb_errors > config_.usb_max_errors) {
            usb_congested_now = true;
        }
    }

    // State machine for switching
    if (active_source_ == Source::USB) {
        if (usb_congested_now) {
            bandwidth_state_.congestion_frames++;
            bandwidth_state_.recovery_frames = 0;

            if (bandwidth_state_.congestion_frames >= config_.congestion_frames && wifi_available) {
                // Switch to WiFi
                Source old_source = active_source_;
                active_source_ = Source::WiFi;
                bandwidth_state_.usb_congested = true;
                bandwidth_state_.congestion_frames = 0;
                bandwidth_state_.last_switch_reason = "USB Congested";
                bandwidth_state_.last_switch_time = now;  // Start cooldown
                MLOG_INFO("hybrid", "Switching to WiFi (USB congested: latency=%.1fms rate=%.1f)", bandwidth_state_.usb_latency_ms, bandwidth_state_.usb_packet_rate);
                if (switch_callback_) {
                    switch_callback_(old_source, Source::WiFi, "USB Congested");
                }
            }
        } else {
            bandwidth_state_.congestion_frames = 0;
        }
    }
    else if (active_source_ == Source::WiFi) {
        // Check if USB recovered
        bool usb_recovered = usb_available &&
                            bandwidth_state_.usb_latency_ms < config_.usb_recovery_latency_ms &&
                            bandwidth_state_.usb_packet_rate >= config_.usb_min_packet_rate;

        if (usb_recovered) {
            bandwidth_state_.recovery_frames++;
            bandwidth_state_.congestion_frames = 0;

            if (bandwidth_state_.recovery_frames >= config_.recovery_frames) {
                // Switch back to USB
                Source old_source = active_source_;
                active_source_ = Source::USB;
                bandwidth_state_.usb_congested = false;
                bandwidth_state_.recovery_frames = 0;
                bandwidth_state_.last_switch_reason = "USB Recovered";
                bandwidth_state_.last_switch_time = now;  // Start cooldown
                MLOG_INFO("hybrid", "Switching back to USB (recovered: latency=%.1fms rate=%.1f)", bandwidth_state_.usb_latency_ms, bandwidth_state_.usb_packet_rate);
                if (switch_callback_) {
                    switch_callback_(old_source, Source::USB, "USB Recovered");
                }
            }
        } else if (!usb_available && !wifi_available) {
            // Both sources unavailable
            active_source_ = Source::None;
            bandwidth_state_.last_switch_reason = "No Source";
        } else {
            bandwidth_state_.recovery_frames = 0;
        }
    }
    else {
        // Source::None - try to connect
        if (usb_available) {
            active_source_ = Source::USB;
            bandwidth_state_.last_switch_reason = "USB Connected";
        } else if (wifi_available) {
            active_source_ = Source::WiFi;
            bandwidth_state_.last_switch_reason = "WiFi Only";
        }
    }
}

bool HybridReceiver::get_latest_frame(MirrorFrame& out) {
    // Update bandwidth stats and evaluate source switch
    updateBandwidthStats();
    evaluateSourceSwitch();

    // Get frame from WiFi receiver (unified decoder)
    // Both USB and WiFi packets are fed to wifi_receiver_ for decoding
    if (wifi_receiver_) {
        bool got_frame = wifi_receiver_->get_latest_frame(out);
        if (got_frame) {
            std::lock_guard<std::mutex> lock(stats_mtx_);
            bandwidth_state_.last_frame = std::chrono::steady_clock::now();
        }
        return got_frame;
    }

    return false;
}

const char* HybridReceiver::active_source_name() const {
    switch (active_source_) {
        case Source::USB:  return "USB";
        case Source::WiFi: return "WiFi";
        default:           return "None";
    }
}

HybridReceiver::Stats HybridReceiver::getStats() const {
    Stats stats;

    std::lock_guard<std::mutex> lock(stats_mtx_);

    stats.active_source = active_source_;

    // USB stats
    uint64_t now_ms_gs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t last_ms_gs = usb_last_packet_time_.load();
    stats.usb_connected = (last_ms_gs > 0) && ((now_ms_gs - last_ms_gs) < 500ULL);
    stats.usb_packets = usb_receiver_ ? usb_receiver_->packets_received() : 0;
    stats.usb_bytes = usb_receiver_ ? usb_receiver_->bytes_received() : 0;
    stats.usb_packet_rate = bandwidth_state_.usb_packet_rate;
    stats.usb_bandwidth_mbps = bandwidth_state_.usb_bandwidth_mbps;
    stats.usb_latency_ms = bandwidth_state_.usb_latency_ms;
    stats.usb_errors = bandwidth_state_.usb_errors;

    // WiFi stats
    stats.wifi_packets = wifi_receiver_ ? wifi_receiver_->packets_received() : 0;
    stats.wifi_bytes = wifi_receiver_ ? wifi_receiver_->bytes_received() : 0;
    stats.wifi_packet_rate = bandwidth_state_.wifi_packet_rate;
    stats.wifi_bandwidth_mbps = bandwidth_state_.wifi_bandwidth_mbps;

    // Frame stats
    stats.frames_decoded = wifi_receiver_ ? wifi_receiver_->frames_decoded() : 0;
    stats.decode_fps = bandwidth_state_.decode_fps;

    // Congestion state
    stats.usb_congested = bandwidth_state_.usb_congested;
    stats.congestion_count = bandwidth_state_.congestion_frames;
    stats.recovery_count = bandwidth_state_.recovery_frames;
    stats.last_switch_reason = bandwidth_state_.last_switch_reason;

    return stats;
}

// Legacy accessors
uint64_t HybridReceiver::usb_packets() const {
    return usb_receiver_ ? usb_receiver_->packets_received() : 0;
}

uint64_t HybridReceiver::usb_bytes() const {
    return usb_receiver_ ? usb_receiver_->bytes_received() : 0;
}

uint64_t HybridReceiver::wifi_packets() const {
    return wifi_receiver_ ? wifi_receiver_->packets_received() : 0;
}

uint64_t HybridReceiver::wifi_bytes() const {
    return wifi_receiver_ ? wifi_receiver_->bytes_received() : 0;
}

uint64_t HybridReceiver::frames_decoded() const {
    return wifi_receiver_ ? wifi_receiver_->frames_decoded() : 0;
}

bool HybridReceiver::usb_connected() const {
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t last_ms = usb_last_packet_time_.load();
    return (last_ms > 0) && ((now_ms - last_ms) < 500ULL);
}

void HybridReceiver::feed_usb_data(const uint8_t* data, size_t len) {
    // Forward USB video data to WiFi receiver for decoding (unified decoder)
    if (wifi_receiver_) {
        wifi_receiver_->feed_rtp_packet(data, len);
    }

    // Update USB packet timing
    usb_last_packet_time_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    // Only promote to USB source if not in cooldown
    {
        std::lock_guard<std::mutex> lock(stats_mtx_);
        auto now_feed = std::chrono::steady_clock::now();
        auto elapsed_feed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now_feed - bandwidth_state_.last_switch_time).count();
        bool in_cooldown = (bandwidth_state_.last_switch_time.time_since_epoch().count() > 0) &&
                           (elapsed_feed < config_.switch_cooldown_ms);
        if (!in_cooldown) {
            Source cur = active_source_.load();
            if (cur == Source::None || cur == Source::WiFi) {
                active_source_.store(Source::USB);
                bandwidth_state_.last_switch_reason = "USB Data Received";
            }
        }
    }
}

} // namespace gui
