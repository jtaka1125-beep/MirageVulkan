#include "bandwidth_monitor.hpp"
#include <cstdio>
#include "mirage_log.hpp"

namespace gui {

BandwidthMonitor::BandwidthMonitor() {
    auto now = std::chrono::steady_clock::now();
    last_usb_activity_ = now;
    last_wifi_activity_ = now;
    last_update_ = now;
}

void BandwidthMonitor::recordUsbSend(size_t bytes) {
    usb_bytes_sent_.fetch_add(bytes);
    std::lock_guard<std::mutex> lock(usb_mutex_);
    last_usb_activity_ = std::chrono::steady_clock::now();
}

void BandwidthMonitor::recordUsbRecv(size_t bytes) {
    usb_bytes_recv_.fetch_add(bytes);
    std::lock_guard<std::mutex> lock(usb_mutex_);
    last_usb_activity_ = std::chrono::steady_clock::now();
}

void BandwidthMonitor::recordPingRtt(float rtt_ms) {
    last_ping_rtt_.store(rtt_ms);
}

void BandwidthMonitor::recordWifiRecv(size_t bytes) {
    wifi_bytes_recv_.fetch_add(bytes);
    std::lock_guard<std::mutex> lock(wifi_mutex_);
    last_wifi_activity_ = std::chrono::steady_clock::now();
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

        auto since_activity = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_usb_activity_).count();
        usb_stats_.is_alive = (since_activity < ALIVE_TIMEOUT_MS);

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

        auto since_activity = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_wifi_activity_).count();
        wifi_stats_.is_alive = (since_activity < ALIVE_TIMEOUT_MS);

        prev_wifi_bytes_recv_ = current_recv;
    }

    last_update_ = now;
}

BandwidthMonitor::UsbStats BandwidthMonitor::getUsbStats() {
    updateStats();
    return usb_stats_;
}

BandwidthMonitor::WifiStats BandwidthMonitor::getWifiStats() {
    updateStats();
    return wifi_stats_;
}

void BandwidthMonitor::reset() {
    usb_bytes_sent_.store(0);
    usb_bytes_recv_.store(0);
    last_ping_rtt_.store(0.0f);
    wifi_bytes_recv_.store(0);
    wifi_packet_loss_.store(0.0f);

    auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(usb_mutex_);
        last_usb_activity_ = now;
    }
    {
        std::lock_guard<std::mutex> lock(wifi_mutex_);
        last_wifi_activity_ = now;
    }

    prev_usb_bytes_sent_ = 0;
    prev_usb_bytes_recv_ = 0;
    prev_wifi_bytes_recv_ = 0;
    last_update_ = now;

    usb_stats_ = UsbStats{};
    wifi_stats_ = WifiStats{};
}

} // namespace gui
