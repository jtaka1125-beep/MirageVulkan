#pragma once
#include <cstdint>

#include <atomic>
#include <chrono>
#include <mutex>

namespace gui {

/**
 * Monitors bandwidth usage and health for USB and WiFi paths.
 * Used by RouteController to make routing decisions.
 */
class BandwidthMonitor {
public:
    struct UsbStats {
        float bandwidth_mbps = 0.0f;     // Current bandwidth usage
        float ping_rtt_ms = 0.0f;        // Last ping RTT
        bool is_congested = false;       // Bandwidth > threshold or RTT high
        bool is_alive = false;           // Recently received data
    };

    struct WifiStats {
        float bandwidth_mbps = 0.0f;     // UDP receive bandwidth
        float packet_loss_rate = 0.0f;   // Estimated packet loss (0-1)
        bool is_alive = false;           // Recently received data
    };

    BandwidthMonitor();

    // Called by USB sender/receiver
    void recordUsbSend(size_t bytes);
    void recordUsbRecv(size_t bytes);
    void recordPingRtt(float rtt_ms);

    // Called by MirrorReceiver
    void recordWifiRecv(size_t bytes);
    void recordWifiPacketLoss(float rate);

    // Get current stats (call periodically, e.g., every second)
    UsbStats getUsbStats();
    WifiStats getWifiStats();

    // Update calculated stats (call periodically, e.g., every second)
    void updateStats();

    // Reset all counters
    void reset();

private:

    // USB metrics
    std::atomic<uint64_t> usb_bytes_sent_{0};
    std::atomic<uint64_t> usb_bytes_recv_{0};
    std::atomic<float> last_ping_rtt_{0.0f};
    // ISSUE-23: atomic nanosecond timestamps replace time_point+mutex in hot path
    std::atomic<int64_t> last_usb_activity_ns_{0};
    mutable std::mutex usb_mutex_;

    // WiFi metrics
    std::atomic<uint64_t> wifi_bytes_recv_{0};
    std::atomic<float> wifi_packet_loss_{0.0f};
    std::atomic<int64_t> last_wifi_activity_ns_{0};
    mutable std::mutex wifi_mutex_;

    // Calculated stats (updated by updateStats)
    UsbStats usb_stats_;
    WifiStats wifi_stats_;
    std::chrono::steady_clock::time_point last_update_;
    uint64_t prev_usb_bytes_sent_ = 0;
    uint64_t prev_usb_bytes_recv_ = 0;
    uint64_t prev_wifi_bytes_recv_ = 0;

    // Thresholds
    static constexpr float USB_CONGESTION_THRESHOLD_MBPS = 25.0f;
    static constexpr float USB_RTT_THRESHOLD_MS = 50.0f;
    static constexpr float WIFI_LOSS_THRESHOLD = 0.1f;  // 10%

    // WiFi/USB は瞬断しがちなので少し長め（特に WiFi 側）
    static constexpr int ALIVE_TIMEOUT_MS = 30000;  // 30s grace
};

} // namespace gui
