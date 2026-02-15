// =============================================================================
// Unit tests for FrameDispatcher (src/frame_dispatcher.hpp)
// =============================================================================
// Tests device registration, frame dispatching, and EventBus integration.
// =============================================================================
#include <gtest/gtest.h>
#include <thread>
#include "frame_dispatcher.hpp"

using namespace mirage;

// ===========================================================================
// Helper: use a fresh (non-singleton) EventBus + FrameDispatcher per test
// ===========================================================================
// FrameDispatcher uses the global bus(), so we test through the global singleton.
// Each test creates a fresh FrameDispatcher instance for isolation.

class FrameDispatcherTest : public ::testing::Test {
protected:
    FrameDispatcher fd;
};

// ===========================================================================
// Device registration
// ===========================================================================
TEST_F(FrameDispatcherTest, RegisterDevice) {
    fd.registerDevice("dev-1", "Pixel 7", "usb");
    EXPECT_TRUE(fd.isKnownDevice("dev-1"));
}

TEST_F(FrameDispatcherTest, UnknownDeviceNotKnown) {
    EXPECT_FALSE(fd.isKnownDevice("nonexistent"));
}

TEST_F(FrameDispatcherTest, RegisterDeviceIdempotent) {
    int connect_count = 0;
    auto sub = bus().subscribe<DeviceConnectedEvent>(
        [&](const DeviceConnectedEvent&) { connect_count++; });

    fd.registerDevice("dev-A", "Device A", "usb");
    fd.registerDevice("dev-A", "Device A", "usb");  // duplicate

    // Should only fire event once
    EXPECT_EQ(connect_count, 1);
    EXPECT_TRUE(fd.isKnownDevice("dev-A"));
}

TEST_F(FrameDispatcherTest, RegisterMultipleDevices) {
    fd.registerDevice("dev-1", "Device 1", "usb");
    fd.registerDevice("dev-2", "Device 2", "wifi");
    fd.registerDevice("dev-3", "Device 3", "usb");

    EXPECT_TRUE(fd.isKnownDevice("dev-1"));
    EXPECT_TRUE(fd.isKnownDevice("dev-2"));
    EXPECT_TRUE(fd.isKnownDevice("dev-3"));
    EXPECT_FALSE(fd.isKnownDevice("dev-4"));
}

// ===========================================================================
// DeviceConnectedEvent published on register
// ===========================================================================
TEST_F(FrameDispatcherTest, RegisterPublishesConnectedEvent) {
    std::string received_id, received_name, received_type;
    auto sub = bus().subscribe<DeviceConnectedEvent>(
        [&](const DeviceConnectedEvent& e) {
            received_id = e.device_id;
            received_name = e.display_name;
            received_type = e.connection_type;
        });

    fd.registerDevice("dev-X", "My Phone", "wifi");

    EXPECT_EQ(received_id, "dev-X");
    EXPECT_EQ(received_name, "My Phone");
    EXPECT_EQ(received_type, "wifi");
}

// ===========================================================================
// dispatchFrame auto-registers new devices
// ===========================================================================
TEST_F(FrameDispatcherTest, DispatchFrameAutoRegisters) {
    uint8_t dummy_rgba[4] = {255, 0, 0, 255};

    EXPECT_FALSE(fd.isKnownDevice("auto-dev"));
    fd.dispatchFrame("auto-dev", dummy_rgba, 1, 1, 0);
    EXPECT_TRUE(fd.isKnownDevice("auto-dev"));
}

TEST_F(FrameDispatcherTest, DispatchFrameAutoRegisterOnlyOnce) {
    uint8_t dummy_rgba[4] = {0, 0, 0, 0};
    int connect_count = 0;
    auto sub = bus().subscribe<DeviceConnectedEvent>(
        [&](const DeviceConnectedEvent&) { connect_count++; });

    fd.dispatchFrame("once-dev", dummy_rgba, 1, 1, 0);
    fd.dispatchFrame("once-dev", dummy_rgba, 1, 1, 1);
    fd.dispatchFrame("once-dev", dummy_rgba, 1, 1, 2);

    // auto-register event should fire only for the first frame
    EXPECT_EQ(connect_count, 1);
}

// ===========================================================================
// dispatchFrame publishes FrameReadyEvent
// ===========================================================================
TEST_F(FrameDispatcherTest, DispatchFramePublishesEvent) {
    uint8_t rgba[16] = {};
    std::string received_device;
    int received_w = 0, received_h = 0;
    uint64_t received_frame_id = 999;

    auto sub = bus().subscribe<FrameReadyEvent>(
        [&](const FrameReadyEvent& e) {
            received_device = e.device_id;
            received_w = e.width;
            received_h = e.height;
            received_frame_id = e.frame_id;
        });

    fd.dispatchFrame("dev-1", rgba, 2, 2, 42);

    EXPECT_EQ(received_device, "dev-1");
    EXPECT_EQ(received_w, 2);
    EXPECT_EQ(received_h, 2);
    EXPECT_EQ(received_frame_id, 42u);
}

// ===========================================================================
// dispatchDisconnect removes device
// ===========================================================================
TEST_F(FrameDispatcherTest, DispatchDisconnectRemovesDevice) {
    fd.registerDevice("dev-1", "Device 1");
    EXPECT_TRUE(fd.isKnownDevice("dev-1"));

    fd.dispatchDisconnect("dev-1");
    EXPECT_FALSE(fd.isKnownDevice("dev-1"));
}

TEST_F(FrameDispatcherTest, DispatchDisconnectPublishesEvent) {
    fd.registerDevice("dev-1", "Device 1");

    std::string disconnected_id;
    auto sub = bus().subscribe<DeviceDisconnectedEvent>(
        [&](const DeviceDisconnectedEvent& e) {
            disconnected_id = e.device_id;
        });

    fd.dispatchDisconnect("dev-1");
    EXPECT_EQ(disconnected_id, "dev-1");
}

TEST_F(FrameDispatcherTest, DisconnectUnknownDeviceDoesNotCrash) {
    // Should not throw or crash
    EXPECT_NO_THROW(fd.dispatchDisconnect("never-registered"));
}

// ===========================================================================
// dispatchStatus publishes DeviceStatusEvent
// ===========================================================================
TEST_F(FrameDispatcherTest, DispatchStatusPublishesEvent) {
    std::string received_id;
    float received_fps = 0, received_latency = 0, received_bw = 0;
    int received_status = -1;

    auto sub = bus().subscribe<DeviceStatusEvent>(
        [&](const DeviceStatusEvent& e) {
            received_id = e.device_id;
            received_status = e.status;
            received_fps = e.fps;
            received_latency = e.latency_ms;
            received_bw = e.bandwidth_mbps;
        });

    fd.dispatchStatus("dev-1", 1, 30.0f, 5.5f, 12.3f);

    EXPECT_EQ(received_id, "dev-1");
    EXPECT_EQ(received_status, 1);
    EXPECT_FLOAT_EQ(received_fps, 30.0f);
    EXPECT_FLOAT_EQ(received_latency, 5.5f);
    EXPECT_FLOAT_EQ(received_bw, 12.3f);
}

// ===========================================================================
// Re-register after disconnect
// ===========================================================================
TEST_F(FrameDispatcherTest, ReRegisterAfterDisconnect) {
    fd.registerDevice("dev-1", "Phone");
    fd.dispatchDisconnect("dev-1");
    EXPECT_FALSE(fd.isKnownDevice("dev-1"));

    // Re-register should work
    fd.registerDevice("dev-1", "Phone Reconnected", "wifi");
    EXPECT_TRUE(fd.isKnownDevice("dev-1"));
}

// ===========================================================================
// Thread safety: concurrent dispatch doesn't crash
// ===========================================================================
TEST_F(FrameDispatcherTest, ConcurrentDispatchNoDataRace) {
    uint8_t rgba[4] = {};
    auto sub = bus().subscribe<FrameReadyEvent>(
        [](const FrameReadyEvent&) {});

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; i++) {
        threads.emplace_back([&, i]() {
            std::string dev = "dev-" + std::to_string(i);
            for (int j = 0; j < 100; j++) {
                fd.dispatchFrame(dev, rgba, 1, 1, j);
            }
        });
    }
    for (auto& t : threads) t.join();

    // All 8 devices should be registered
    for (int i = 0; i < 8; i++) {
        EXPECT_TRUE(fd.isKnownDevice("dev-" + std::to_string(i)));
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
