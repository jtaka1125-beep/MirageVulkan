#include "bandwidth_monitor.hpp"
#include <cstdio>
#include "mirage_log.hpp"

namespace gui {

BandwidthMonitor::BandwidthMonitor() {
    auto now = std::chrono::steady_clock::now();
    // ISSUE-23: store as atomic nanoseconds
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    last_usb_activity_ns_.store(now_ns);
    last_wifi_activity_ns_.store(now_ns);
    last_update_ = now;
}

void BandwidthMonitor::recordUsbSend(size_t bytes) {
    usb_bytes_sent_.fetch_add(bytes);
    // ISSUE-23: lock-free timestamp update
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    last_usb_activity_ns_.store(ns, std::memory_order_relaxed);
}

void BandwidthMonitor::recordUsbRecv(size_t bytes) {
    usb_bytes_recv_.fetch_add(bytes);
    // ISSUE-23: lock-free timestamp update
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    last_usb_activity_ns_.store(ns, std::memory_order_relaxed);
}

void BandwidthMonitor::recordPingRtt(float rtt_ms) {
    last_ping_rtt_.store(rtt_ms);
}

void BandwidthMonitor::recordWifiRecv(size_t bytes) {
    wifi_bytes_recv_.fetch_add(bytes);
    // ISSUE-23: lock-free timestamp update
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    last_wifi_activity_ns_.store(ns, std::memory_order_relaxed);
}

void BandwidthMonitor::recordWifiPacketLoss(float rate) {
    wifi_packet_loss_.store(rate);
}

void BandwidthMonitor::updateStats() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_update_).count();

    if (elapsed_ms < 100) return;  // Don't update too frequently

    float elapsed_sec = elapsed_ms / 1000.0f;

    // USB stats
    {
        std::lock_guard<std::mutex> lock(usb_mutex_);

        uint64_t current_sent = usb_bytes_sent_.load();
        uint64_t current_recv = usb_bytes_recv_.load();
        uint64_t new_sent = current_sent - prev_usb_bytes_sent_;
        uint64_t new_recv = current_recv - prev_usb_bytes_recv_;

        float send_mbps = (new_sent * 8.0f / 1000000.0f) / elapsed_sec;
        float recv_mbps = (new_recv * 8.0f / 1000000.0f) / elapsed_sec;
        usb_stats_.bandwidth_mbps = send_mbps + recv_mbps;

        usb_stats_.ping_rtt_ms = last_ping_rtt_.load();

        usb_stats_.is_congested =
            (usb_stats_.bandwidth_mbps > USB_CONGESTION_THRESHOLD_MBPS) ||
            (usb_stats_.ping_rtt_ms > USB_RTT_THRESHOLD_MS);

        // ISSUE-23: read atomic timestamp without extra lock
        auto usb_act_ns = last_usb_activity_ns_.load(std::memory_order_relaxed);
        auto now_ns_usb = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        usb_stats_.is_alive = ((now_ns_usb - usb_act_ns) / 1000000LL < ALIVE_TIMEOUT_MS);

        prev_usb_bytes_sent_ = current_sent;
        prev_usb_bytes_recv_ = current_recv;
    }

    // WiFi stats
    {
        std::lock_guard<std::mutex> lock(wifi_mutex_);

        uint64_t current_recv = wifi_bytes_recv_.load();
        uint64_t new_recv = current_recv - prev_wifi_bytes_recv_;

        wifi_stats_.bandwidth_mbps = (new_recv * 8.0f / 1000000.0f) / elapsed_sec;
        wifi_stats_.packet_loss_rate = wifi_packet_loss_.load();

        // ISSUE-23: read atomic timestamp without extra lock
        auto wifi_act_ns = last_wifi_activity_ns_.load(std::memory_order_relaxed);
        auto now_ns_wifi = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        wifi_stats_.is_alive = ((now_ns_wifi - wifi_act_ns) / 1000000LL < ALIVE_TIMEOUT_MS);

        prev_wifi_bytes_recv_ = current_recv;
    }

    last_update_ = now;
}

BandwidthMonitor::UsbStats BandwidthMonitor::getUsbStats() {
    updateStats();
    std::lock_guard<std::mutex> lock(usb_mutex_);  // ISSUE-4: TOCTOU fix
    return usb_stats_;
}

BandwidthMonitor::WifiStats BandwidthMonitor::getWifiStats() {
    updateStats();
    std::lock_guard<std::mutex> lock(wifi_mutex_);  // ISSUE-4: TOCTOU fix
    return wifi_stats_;
}

void BandwidthMonitor::reset() {
    usb_bytes_sent_.store(0);
    usb_bytes_recv_.store(0);
    last_ping_rtt_.store(0.0f);
    wifi_bytes_recv_.store(0);
    wifi_packet_loss_.store(0.0f);

    auto now = std::chrono::steady_clock::now();

    // ISSUE-23: atomic stores - no mutex needed
    auto reset_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    last_usb_activity_ns_.store(reset_ns);
    last_wifi_activity_ns_.store(reset_ns);

    prev_usb_bytes_sent_ = 0;
    prev_usb_bytes_recv_ = 0;
    prev_wifi_bytes_recv_ = 0;
    last_update_ = now;

    usb_stats_ = UsbStats{};
    wifi_stats_ = WifiStats{};
}

} // namespace gui
