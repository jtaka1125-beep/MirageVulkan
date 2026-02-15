#pragma once
#include "mirror_receiver.hpp"
#include "usb_video_receiver.hpp"
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>

namespace gui {

/**
 * Hybrid Video Receiver with Bandwidth Monitoring
 *
 * Architecture:
 *   - Commands: Always via USB (low latency) - handled by UsbCommandSender
 *   - Video: USB priority, auto-switch to WiFi when bandwidth congested
 *
 * Bandwidth monitoring:
 *   - Tracks USB packet rate, latency, and errors
 *   - Switches to WiFi when USB is congested (high latency or packet loss)
 *   - Switches back to USB when conditions improve
 *
 * Note: Android outputs to both USB and WiFi simultaneously.
 *       PC decides which source to use for display.
 */
class HybridReceiver {
public:
    enum class Source { None, USB, WiFi };

    // Bandwidth/quality thresholds
    struct Config {
        // Switch USB -> WiFi thresholds
        float usb_max_latency_ms = 50.0f;       // Max acceptable USB latency
        float usb_min_packet_rate = 20.0f;      // Min packets/sec expected
        int   usb_max_errors = 5;               // Max errors before switch
        int   congestion_frames = 30;           // Frames of congestion before switch

        // Switch WiFi -> USB thresholds (hysteresis)
        float usb_recovery_latency_ms = 30.0f;  // USB latency to consider recovered
        int   recovery_frames = 60;             // Good frames before switching back

        // Anti-flapping: cooldown period after switch (milliseconds)
        int   switch_cooldown_ms = 3000;        // 3 seconds cooldown after any switch
    };

    // Real-time stats for UI
    struct Stats {
        Source active_source = Source::None;

        // USB stats
        bool     usb_connected = false;
        uint64_t usb_packets = 0;
        uint64_t usb_bytes = 0;
        float    usb_packet_rate = 0.0f;    // packets/sec
        float    usb_bandwidth_mbps = 0.0f; // MB/s
        float    usb_latency_ms = 0.0f;     // estimated latency
        int      usb_errors = 0;

        // WiFi stats
        uint64_t wifi_packets = 0;
        uint64_t wifi_bytes = 0;
        float    wifi_packet_rate = 0.0f;
        float    wifi_bandwidth_mbps = 0.0f;

        // Decoded frames
        uint64_t frames_decoded = 0;
        float    decode_fps = 0.0f;

        // Congestion status
        bool     usb_congested = false;
        int      congestion_count = 0;      // frames in congestion state
        int      recovery_count = 0;        // frames in recovery state

        // Last switch reason
        const char* last_switch_reason = "None";
    };

    HybridReceiver();
    ~HybridReceiver();

    // Configuration
    void setConfig(const Config& cfg) { config_ = cfg; }
    Config& config() { return config_; }
    const Config& config() const { return config_; }

    // Start receiving (WiFi port, USB auto-detected)
    bool start(uint16_t wifi_port = 60000);
    void stop();

    bool running() const { return running_; }

    // Get latest frame (from active source)
    bool get_latest_frame(MirrorFrame& out);

    // Current active source
    Source active_source() const { return active_source_; }
    const char* active_source_name() const;

    // Source switch callback (for logging)
    using SwitchCallback = std::function<void(Source from, Source to, const char* reason)>;
    void setSwitchCallback(SwitchCallback cb) { switch_callback_ = std::move(cb); }

    // Get real-time stats for UI
    Stats getStats() const;

    // Feed USB video data from external source (e.g., MultiUsbCommandSender)
    void feed_usb_data(const uint8_t* data, size_t len);

    // Legacy accessors
    uint64_t usb_packets() const;
    uint64_t usb_bytes() const;
    uint64_t wifi_packets() const;
    uint64_t wifi_bytes() const;
    uint64_t frames_decoded() const;
    bool usb_connected() const;

private:
    void updateBandwidthStats();
    void evaluateSourceSwitch();

    bool running_ = false;
    std::atomic<Source> active_source_{Source::None};
    Config config_;

    std::unique_ptr<UsbVideoReceiver> usb_receiver_;
    std::unique_ptr<MirrorReceiver> wifi_receiver_;

    // Bandwidth monitoring
    struct BandwidthState {
        // Timing
        std::chrono::steady_clock::time_point last_update;
        std::chrono::steady_clock::time_point last_usb_packet;
        std::chrono::steady_clock::time_point last_wifi_packet;
        std::chrono::steady_clock::time_point last_frame;

        // Previous values for rate calculation
        uint64_t prev_usb_packets = 0;
        uint64_t prev_usb_bytes = 0;
        uint64_t prev_wifi_packets = 0;
        uint64_t prev_wifi_bytes = 0;
        uint64_t prev_frames = 0;

        // Calculated rates
        float usb_packet_rate = 0.0f;
        float usb_bandwidth_mbps = 0.0f;
        float wifi_packet_rate = 0.0f;
        float wifi_bandwidth_mbps = 0.0f;
        float decode_fps = 0.0f;

        // Latency estimation (time since last packet)
        float usb_latency_ms = 0.0f;

        // Error tracking
        int usb_errors = 0;

        // Congestion state machine
        bool usb_congested = false;
        int congestion_frames = 0;
        int recovery_frames = 0;

        // Anti-flapping: time of last switch
        std::chrono::steady_clock::time_point last_switch_time;
        bool in_cooldown = false;

        // Switch reason
        const char* last_switch_reason = "None";
    };

    mutable std::mutex stats_mtx_;
    BandwidthState bandwidth_state_;

    // Track which source provided last packet
    std::atomic<uint64_t> usb_last_packet_time_{0};
    std::atomic<uint64_t> wifi_last_packet_time_{0};

    // Switch callback
    SwitchCallback switch_callback_;
};

} // namespace gui
