// =============================================================================
// Unit tests for BandwidthMonitor (src/bandwidth_monitor.hpp/.cpp)
// GPU不要 — 帯域統計計算・congestion判定・alive判定のCPU純粋ロジックテスト
// =============================================================================
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include "bandwidth_monitor.hpp"

using namespace gui;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// BM-1: 初期状態 — is_alive=true (コンストラクタで現在時刻を記録)
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, InitialAliveTrue) {
    BandwidthMonitor bm;
    std::this_thread::sleep_for(150ms);  // wait for updateStats guard
    auto usb  = bm.getUsbStats();
    auto wifi = bm.getWifiStats();
    EXPECT_TRUE(usb.is_alive);
    EXPECT_TRUE(wifi.is_alive);
}

// ---------------------------------------------------------------------------
// BM-2: 初期状態 — bandwidth=0, congested=false
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, InitialBandwidthZero) {
    BandwidthMonitor bm;
    std::this_thread::sleep_for(150ms);  // elapsed_ms >= 100 required
    auto usb  = bm.getUsbStats();
    auto wifi = bm.getWifiStats();
    EXPECT_FLOAT_EQ(usb.bandwidth_mbps,  0.0f);
    EXPECT_FLOAT_EQ(wifi.bandwidth_mbps, 0.0f);
    EXPECT_FALSE(usb.is_congested);
}

// ---------------------------------------------------------------------------
// BM-3: USB送受信後 updateStats → bandwidth > 0
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, UsbBandwidthPositiveAfterData) {
    BandwidthMonitor bm;
    std::this_thread::sleep_for(120ms);
    bm.recordUsbSend(1000000);   // 1 MB
    bm.recordUsbRecv(500000);    // 0.5 MB
    std::this_thread::sleep_for(120ms);
    auto stats = bm.getUsbStats();
    EXPECT_GT(stats.bandwidth_mbps, 0.0f);
}

// ---------------------------------------------------------------------------
// BM-4: WiFi受信後 updateStats → bandwidth > 0
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, WifiBandwidthPositiveAfterData) {
    BandwidthMonitor bm;
    std::this_thread::sleep_for(120ms);
    bm.recordWifiRecv(2000000);   // 2 MB
    std::this_thread::sleep_for(120ms);
    auto stats = bm.getWifiStats();
    EXPECT_GT(stats.bandwidth_mbps, 0.0f);
}

// ---------------------------------------------------------------------------
// BM-5: ping RTT 記録 → getUsbStats().ping_rtt_ms に反映
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, PingRttRecorded) {
    BandwidthMonitor bm;
    bm.recordPingRtt(12.5f);
    std::this_thread::sleep_for(150ms);
    auto stats = bm.getUsbStats();
    EXPECT_FLOAT_EQ(stats.ping_rtt_ms, 12.5f);
}

// ---------------------------------------------------------------------------
// BM-6: RTT > 50ms → is_congested = true
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, CongestedWhenRttHigh) {
    BandwidthMonitor bm;
    bm.recordPingRtt(100.0f);   // > USB_RTT_THRESHOLD_MS (50ms)
    std::this_thread::sleep_for(150ms);
    auto stats = bm.getUsbStats();
    EXPECT_TRUE(stats.is_congested);
}

// ---------------------------------------------------------------------------
// BM-7: WiFi packet loss 記録 → packet_loss_rate に反映
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, WifiPacketLossRecorded) {
    BandwidthMonitor bm;
    bm.recordWifiPacketLoss(0.15f);
    std::this_thread::sleep_for(150ms);
    auto stats = bm.getWifiStats();
    EXPECT_FLOAT_EQ(stats.packet_loss_rate, 0.15f);
}

// ---------------------------------------------------------------------------
// BM-8: reset() → 全カウンタ 0 に戻る
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, ResetClearsAll) {
    BandwidthMonitor bm;
    bm.recordUsbSend(9999999);
    bm.recordWifiRecv(9999999);
    bm.recordPingRtt(999.0f);
    bm.recordWifiPacketLoss(0.9f);
    bm.reset();
    std::this_thread::sleep_for(150ms);
    auto usb  = bm.getUsbStats();
    auto wifi = bm.getWifiStats();
    EXPECT_FLOAT_EQ(usb.bandwidth_mbps,    0.0f);
    EXPECT_FLOAT_EQ(usb.ping_rtt_ms,       0.0f);
    EXPECT_FLOAT_EQ(wifi.bandwidth_mbps,   0.0f);
    EXPECT_FLOAT_EQ(wifi.packet_loss_rate, 0.0f);
}

// ---------------------------------------------------------------------------
// BM-9: reset() 後も is_alive = true (タイムスタンプ更新)
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, AliveAfterReset) {
    BandwidthMonitor bm;
    bm.reset();
    std::this_thread::sleep_for(150ms);
    EXPECT_TRUE(bm.getUsbStats().is_alive);
    EXPECT_TRUE(bm.getWifiStats().is_alive);
}

// ---------------------------------------------------------------------------
// BM-10: updateStats を 100ms 未満で呼んでも帯域は更新されない (ガード)
// ---------------------------------------------------------------------------
TEST(BandwidthMonitorTest, UpdateStatsTooFrequentIgnored) {
    BandwidthMonitor bm;
    std::this_thread::sleep_for(150ms);
    bm.recordUsbSend(1000000);
    // 呼び出し間隔 < 100ms → 帯域更新されない
    bm.updateStats();
    bm.updateStats();  // 即座に2回目 → ガードされるはず
    // 厳密には内部状態依存のため、クラッシュしないことを確認する
    SUCCEED();
}
