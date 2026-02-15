// =============================================================================
// Unit tests for RttTracker (src/rtt_tracker.hpp) and
//                BandwidthMonitor (src/bandwidth_monitor.hpp/.cpp)
// =============================================================================
// Tests pure logic: EMA, histogram, latency classification, bandwidth stats.
// No network/USB hardware required.
// =============================================================================
#include <gtest/gtest.h>
#include "rtt_tracker.hpp"
#include "bandwidth_monitor.hpp"

#include <thread>
#include <chrono>

using namespace mirage;

// ===========================================================================
// AtomicEMA
// ===========================================================================
TEST(AtomicEMA, InitialValueIsZero) {
    AtomicEMA ema(0.5);
    EXPECT_DOUBLE_EQ(ema.get(), 0.0);
}

TEST(AtomicEMA, FirstUpdateSetsValue) {
    AtomicEMA ema(0.1);
    ema.update(100.0);
    EXPECT_DOUBLE_EQ(ema.get(), 100.0);  // First value adopted as-is
}

TEST(AtomicEMA, SecondUpdateBlends) {
    AtomicEMA ema(0.5);  // alpha = 0.5
    ema.update(100.0);   // -> 100
    ema.update(200.0);   // -> 100*0.5 + 200*0.5 = 150
    EXPECT_DOUBLE_EQ(ema.get(), 150.0);
}

TEST(AtomicEMA, ConvergesToConstantInput) {
    AtomicEMA ema(0.3);
    for (int i = 0; i < 100; i++) {
        ema.update(50.0);
    }
    EXPECT_NEAR(ema.get(), 50.0, 0.01);
}

TEST(AtomicEMA, Reset) {
    AtomicEMA ema(0.1);
    ema.update(100.0);
    ema.reset();
    EXPECT_DOUBLE_EQ(ema.get(), 0.0);
}

// ===========================================================================
// LatencyHistogram
// ===========================================================================
TEST(LatencyHistogram, EmptyHistogramPercentile) {
    LatencyHistogram h;
    EXPECT_DOUBLE_EQ(h.percentile(50), 0.0);
    EXPECT_EQ(h.total_count(), 0u);
}

TEST(LatencyHistogram, SingleRecord) {
    LatencyHistogram h;
    h.record(3.0);  // -> bucket [0, 5ms)
    EXPECT_EQ(h.total_count(), 1u);
    EXPECT_EQ(h.bucket_count(0), 1u);
}

TEST(LatencyHistogram, BucketBoundaries) {
    LatencyHistogram h;

    // Record values at specific bucket boundaries
    h.record(0.0);    // [0, 5)   -> bucket 0
    h.record(4.9);    // [0, 5)   -> bucket 0
    h.record(5.0);    // [5, 10)  -> bucket 1
    h.record(9.9);    // [5, 10)  -> bucket 1
    h.record(10.0);   // [10, 20) -> bucket 2
    h.record(50.0);   // [50, 100)-> bucket 4
    h.record(200.0);  // [200, 500) -> bucket 6
    h.record(1000.0); // [1000, +inf) -> bucket 8
    h.record(5000.0); // [1000, +inf) -> bucket 8

    EXPECT_EQ(h.bucket_count(0), 2u);  // 0.0, 4.9
    EXPECT_EQ(h.bucket_count(1), 2u);  // 5.0, 9.9
    EXPECT_EQ(h.bucket_count(2), 1u);  // 10.0
    EXPECT_EQ(h.bucket_count(4), 1u);  // 50.0
    EXPECT_EQ(h.bucket_count(6), 1u);  // 200.0
    EXPECT_EQ(h.bucket_count(8), 2u);  // 1000.0, 5000.0
    EXPECT_EQ(h.total_count(), 9u);
}

TEST(LatencyHistogram, PercentileCalculation) {
    LatencyHistogram h;
    // 10 samples in bucket 0 ([0,5ms))
    for (int i = 0; i < 10; i++) h.record(2.0);

    EXPECT_DOUBLE_EQ(h.percentile(50), 5.0);  // p50 falls in bucket 0
    EXPECT_DOUBLE_EQ(h.percentile(99), 5.0);
}

TEST(LatencyHistogram, PercentileMixedBuckets) {
    LatencyHistogram h;
    // 90 samples fast, 10 samples slow
    for (int i = 0; i < 90; i++) h.record(3.0);   // bucket 0
    for (int i = 0; i < 10; i++) h.record(150.0);  // bucket 5 [100,200)

    // p50 should be in fast bucket
    EXPECT_DOUBLE_EQ(h.percentile(50), 5.0);
    // p95 should be in slow bucket
    EXPECT_DOUBLE_EQ(h.percentile(95), 200.0);
}

TEST(LatencyHistogram, BucketCountOutOfRange) {
    LatencyHistogram h;
    EXPECT_EQ(h.bucket_count(100), 0u);
}

TEST(LatencyHistogram, Reset) {
    LatencyHistogram h;
    h.record(10.0);
    h.record(50.0);
    h.reset();
    EXPECT_EQ(h.total_count(), 0u);
}

// ===========================================================================
// RttTracker
// ===========================================================================
TEST(RttTracker, InitialState) {
    RttTracker rtt;
    EXPECT_EQ(rtt.sample_count(), 0u);
    EXPECT_DOUBLE_EQ(rtt.avg_rtt_ms(), 0.0);
    EXPECT_EQ(rtt.classify(), RttTracker::Level::GOOD);
}

TEST(RttTracker, UpdateRecordsStats) {
    RttTracker rtt;
    rtt.update(10.0);

    EXPECT_EQ(rtt.sample_count(), 1u);
    EXPECT_DOUBLE_EQ(rtt.avg_rtt_ms(), 10.0);  // First value
    EXPECT_LE(rtt.min_rtt_ms(), 10.0);
    EXPECT_GE(rtt.max_rtt_ms(), 10.0);
}

TEST(RttTracker, MinMaxTracking) {
    RttTracker rtt;
    rtt.update(50.0);
    rtt.update(10.0);
    rtt.update(100.0);
    rtt.update(5.0);

    EXPECT_DOUBLE_EQ(rtt.min_rtt_ms(), 5.0);
    EXPECT_DOUBLE_EQ(rtt.max_rtt_ms(), 100.0);
}

TEST(RttTracker, ClassifyGood) {
    RttTracker rtt;
    for (int i = 0; i < 100; i++) rtt.update(10.0);
    EXPECT_EQ(rtt.classify(), RttTracker::Level::GOOD);
}

TEST(RttTracker, ClassifyWarning) {
    RttTracker rtt;
    for (int i = 0; i < 100; i++) rtt.update(80.0);
    EXPECT_EQ(rtt.classify(), RttTracker::Level::WARNING);
}

TEST(RttTracker, ClassifyCritical) {
    RttTracker rtt;
    for (int i = 0; i < 100; i++) rtt.update(300.0);
    EXPECT_EQ(rtt.classify(), RttTracker::Level::CRITICAL);
}

TEST(RttTracker, LevelStr) {
    EXPECT_STREQ(RttTracker::level_str(RttTracker::Level::GOOD), "good");
    EXPECT_STREQ(RttTracker::level_str(RttTracker::Level::WARNING), "warning");
    EXPECT_STREQ(RttTracker::level_str(RttTracker::Level::CRITICAL), "critical");
}

TEST(RttTracker, Percentiles) {
    RttTracker rtt;
    for (int i = 0; i < 100; i++) rtt.update(3.0);   // all in bucket [0,5)

    EXPECT_GT(rtt.p50(), 0.0);
    EXPECT_GT(rtt.p95(), 0.0);
    EXPECT_GT(rtt.p99(), 0.0);
}

TEST(RttTracker, PingPongRoundTrip) {
    RttTracker rtt;
    rtt.record_ping_sent(1);

    // Simulate some time passing
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto result = rtt.record_pong_recv(1);
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(*result, 0.0);  // RTT should be positive
}

TEST(RttTracker, PongWithoutPingReturnsNullopt) {
    RttTracker rtt;
    auto result = rtt.record_pong_recv(42);
    EXPECT_FALSE(result.has_value());
}

TEST(RttTracker, DuplicatePongReturnsNullopt) {
    RttTracker rtt;
    rtt.record_ping_sent(10);

    auto first = rtt.record_pong_recv(10);
    EXPECT_TRUE(first.has_value());

    // Second pong for same seq should fail
    auto second = rtt.record_pong_recv(10);
    EXPECT_FALSE(second.has_value());
}

TEST(RttTracker, ClearRemovesPendingPings) {
    RttTracker rtt;
    rtt.record_ping_sent(1);
    rtt.record_ping_sent(2);
    rtt.record_ping_sent(3);

    rtt.clear();

    EXPECT_FALSE(rtt.record_pong_recv(1).has_value());
    EXPECT_FALSE(rtt.record_pong_recv(2).has_value());
    EXPECT_FALSE(rtt.record_pong_recv(3).has_value());
}

TEST(RttTracker, ResetClearsEverything) {
    RttTracker rtt;
    rtt.update(100.0);
    rtt.update(200.0);
    rtt.record_ping_sent(1);

    rtt.reset();

    EXPECT_EQ(rtt.sample_count(), 0u);
    EXPECT_DOUBLE_EQ(rtt.avg_rtt_ms(), 0.0);
    EXPECT_FALSE(rtt.record_pong_recv(1).has_value());
}

// ===========================================================================
// BandwidthMonitor
// ===========================================================================
TEST(BandwidthMonitor, InitialStateClean) {
    gui::BandwidthMonitor bw;
    auto usb = bw.getUsbStats();
    auto wifi = bw.getWifiStats();

    EXPECT_FLOAT_EQ(usb.ping_rtt_ms, 0.0f);
    EXPECT_FALSE(usb.is_congested);
    EXPECT_FLOAT_EQ(wifi.packet_loss_rate, 0.0f);
}

TEST(BandwidthMonitor, RecordPingRtt) {
    gui::BandwidthMonitor bw;
    bw.recordPingRtt(25.5f);

    // Need to wait >100ms for updateStats to run
    std::this_thread::sleep_for(std::chrono::milliseconds(110));

    auto usb = bw.getUsbStats();
    EXPECT_FLOAT_EQ(usb.ping_rtt_ms, 25.5f);
}

TEST(BandwidthMonitor, CongestionDetectionByRtt) {
    gui::BandwidthMonitor bw;
    // RTT above threshold (50ms) triggers congestion
    bw.recordPingRtt(100.0f);

    std::this_thread::sleep_for(std::chrono::milliseconds(110));

    auto usb = bw.getUsbStats();
    EXPECT_TRUE(usb.is_congested);
}

TEST(BandwidthMonitor, NoCongestionBelowThreshold) {
    gui::BandwidthMonitor bw;
    bw.recordPingRtt(10.0f);

    std::this_thread::sleep_for(std::chrono::milliseconds(110));

    auto usb = bw.getUsbStats();
    EXPECT_FALSE(usb.is_congested);
}

TEST(BandwidthMonitor, RecordWifiPacketLoss) {
    gui::BandwidthMonitor bw;
    bw.recordWifiPacketLoss(0.15f);

    std::this_thread::sleep_for(std::chrono::milliseconds(110));

    auto wifi = bw.getWifiStats();
    EXPECT_FLOAT_EQ(wifi.packet_loss_rate, 0.15f);
}

TEST(BandwidthMonitor, UsbActivityKeepsAlive) {
    gui::BandwidthMonitor bw;
    bw.recordUsbRecv(1000);

    std::this_thread::sleep_for(std::chrono::milliseconds(110));

    auto usb = bw.getUsbStats();
    EXPECT_TRUE(usb.is_alive);
}

TEST(BandwidthMonitor, Reset) {
    gui::BandwidthMonitor bw;
    bw.recordUsbSend(10000);
    bw.recordUsbRecv(20000);
    bw.recordPingRtt(100.0f);
    bw.recordWifiRecv(30000);
    bw.recordWifiPacketLoss(0.5f);

    bw.reset();

    std::this_thread::sleep_for(std::chrono::milliseconds(110));

    auto usb = bw.getUsbStats();
    auto wifi = bw.getWifiStats();
    EXPECT_FLOAT_EQ(usb.ping_rtt_ms, 0.0f);
    EXPECT_FLOAT_EQ(wifi.packet_loss_rate, 0.0f);
}

// ===========================================================================
// Threshold constants sanity
// ===========================================================================
TEST(RttTracker, ThresholdConstants) {
    EXPECT_DOUBLE_EQ(RttTracker::WARNING_THRESHOLD_MS, 50.0);
    EXPECT_DOUBLE_EQ(RttTracker::CRITICAL_THRESHOLD_MS, 200.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
