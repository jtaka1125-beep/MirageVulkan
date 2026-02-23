// =============================================================================
// Unit tests for BandwidthMonitor (src/bandwidth_monitor.hpp/.cpp)
// GPU不要 — 帯域計算・輻輳判定・alive判定のCPUロジックテスト
// =============================================================================
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include "bandwidth_monitor.hpp"

using namespace gui;

// ---------------------------------------------------------------------------
// B-1: 初期状態は全フィールドゼロ / not alive (直後はalive)
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, InitialStatsZero) {
    BandwidthMonitor bm;
    bm.reset();
    // after reset, counters are 0 but alive may be true (just reset)
    auto usb = bm.getUsbStats();
    auto wifi = bm.getWifiStats();
    EXPECT_FLOAT_EQ(usb.bandwidth_mbps, 0.0f);
    EXPECT_FLOAT_EQ(usb.ping_rtt_ms, 0.0f);
    EXPECT_FALSE(usb.is_congested);
    EXPECT_FLOAT_EQ(wifi.bandwidth_mbps, 0.0f);
    EXPECT_FLOAT_EQ(wifi.packet_loss_rate, 0.0f);
}

// ---------------------------------------------------------------------------
// B-2: recordPingRtt → getUsbStats に反映
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, PingRttReflected) {
    BandwidthMonitor bm;
    bm.recordPingRtt(15.5f);
    // Force update by sleeping > 100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    auto stats = bm.getUsbStats();
    EXPECT_FLOAT_EQ(stats.ping_rtt_ms, 15.5f);
}

// ---------------------------------------------------------------------------
// B-3: RTT > 50ms → is_congested = true
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, CongestedOnHighRtt) {
    BandwidthMonitor bm;
    bm.recordPingRtt(60.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_TRUE(bm.getUsbStats().is_congested);
}

// ---------------------------------------------------------------------------
// B-4: RTT < 50ms, bandwidth < 25Mbps → not congested
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, NotCongestedNormal) {
    BandwidthMonitor bm;
    bm.recordPingRtt(10.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_FALSE(bm.getUsbStats().is_congested);
}

// ---------------------------------------------------------------------------
// B-5: recordWifiPacketLoss → getWifiStats に反映
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, WifiPacketLossReflected) {
    BandwidthMonitor bm;
    bm.recordWifiPacketLoss(0.25f);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    auto stats = bm.getWifiStats();
    EXPECT_FLOAT_EQ(stats.packet_loss_rate, 0.25f);
}

// ---------------------------------------------------------------------------
// B-6: recordUsbSend/Recv でバイト累積
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, UsbBytesAccumulate) {
    BandwidthMonitor bm;
    bm.reset();
    bm.recordUsbSend(1000);
    bm.recordUsbRecv(500);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto stats = bm.getUsbStats();
    // bandwidth_mbps > 0 after bytes received in elapsed window
    EXPECT_GT(stats.bandwidth_mbps, 0.0f);
}

// ---------------------------------------------------------------------------
// B-7: recordWifiRecv でバイト累積
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, WifiBytesAccumulate) {
    BandwidthMonitor bm;
    bm.reset();
    bm.recordWifiRecv(2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto stats = bm.getWifiStats();
    EXPECT_GT(stats.bandwidth_mbps, 0.0f);
}

// ---------------------------------------------------------------------------
// B-8: reset() → 全カウンタリセット
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, ResetClearsStats) {
    BandwidthMonitor bm;
    bm.recordPingRtt(99.0f);
    bm.recordWifiPacketLoss(0.9f);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    bm.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    auto usb = bm.getUsbStats();
    auto wifi = bm.getWifiStats();
    EXPECT_FLOAT_EQ(usb.ping_rtt_ms, 0.0f);
    EXPECT_FLOAT_EQ(wifi.packet_loss_rate, 0.0f);
    EXPECT_FLOAT_EQ(usb.bandwidth_mbps, 0.0f);
    EXPECT_FLOAT_EQ(wifi.bandwidth_mbps, 0.0f);
}

// ---------------------------------------------------------------------------
// B-9: updateStats は 100ms 以内に連呼しても二重計上しない
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, UpdateStatsNotDoubleCount) {
    BandwidthMonitor bm;
    bm.reset();
    bm.recordUsbSend(10000);
    bm.updateStats();  // first call
    float bw1 = bm.getUsbStats().bandwidth_mbps;
    bm.recordUsbSend(0);  // no new bytes
    bm.updateStats();     // second call immediately (< 100ms → skipped)
    float bw2 = bm.getUsbStats().bandwidth_mbps;
    // Should be same since second update was skipped
    EXPECT_EQ(bw1, bw2);
}

// ---------------------------------------------------------------------------
// B-10: UsbStats / WifiStats 構造体コピー可能
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, StatsCopyable) {
    BandwidthMonitor bm;
    bm.recordPingRtt(5.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    BandwidthMonitor::UsbStats s1 = bm.getUsbStats();
    BandwidthMonitor::UsbStats s2 = s1;
    EXPECT_FLOAT_EQ(s1.ping_rtt_ms, s2.ping_rtt_ms);
}
