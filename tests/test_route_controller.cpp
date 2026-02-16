// =============================================================================
// MirageVulkan - RouteController Unit Tests
// =============================================================================

#include <gtest/gtest.h>
#include "route_controller.hpp"
#include "bandwidth_monitor.hpp"
#include <vector>
#include <string>

using namespace gui;

// =============================================================================
// Test Fixture
// =============================================================================

class RouteControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        controller = std::make_unique<RouteController>();

        // Capture FPS commands
        controller->setFpsCommandCallback([this](const std::string& device_id, int fps) {
            fps_commands.push_back({device_id, fps});
        });

        // Capture route commands
        controller->setRouteCommandCallback([this](const std::string& device_id,
                                                     RouteController::VideoRoute route,
                                                     const std::string& host, int port) {
            route_commands.push_back({device_id, route, host, port});
        });
    }

    struct FpsCommand {
        std::string device_id;
        int fps;
    };

    struct RouteCommand {
        std::string device_id;
        RouteController::VideoRoute route;
        std::string host;
        int port;
    };

    std::unique_ptr<RouteController> controller;
    std::vector<FpsCommand> fps_commands;
    std::vector<RouteCommand> route_commands;

    // Helper: create healthy USB stats
    BandwidthMonitor::UsbStats healthyUsb() {
        BandwidthMonitor::UsbStats stats;
        stats.bandwidth_mbps = 10.0f;
        stats.is_alive = true;
        stats.is_congested = false;
        return stats;
    }

    // Helper: create healthy WiFi stats
    BandwidthMonitor::WifiStats healthyWifi() {
        BandwidthMonitor::WifiStats stats;
        stats.bandwidth_mbps = 50.0f;
        stats.is_alive = true;
        return stats;
    }

    // Helper: create congested USB stats
    BandwidthMonitor::UsbStats congestedUsb() {
        BandwidthMonitor::UsbStats stats;
        stats.bandwidth_mbps = 2.0f;
        stats.is_alive = true;
        stats.is_congested = true;
        return stats;
    }

    // Helper: create dead USB stats
    BandwidthMonitor::UsbStats deadUsb() {
        BandwidthMonitor::UsbStats stats;
        stats.bandwidth_mbps = 0.0f;
        stats.is_alive = false;
        stats.is_congested = false;
        return stats;
    }

    // Helper: create dead WiFi stats
    BandwidthMonitor::WifiStats deadWifi() {
        BandwidthMonitor::WifiStats stats;
        stats.bandwidth_mbps = 0.0f;
        stats.is_alive = false;
        return stats;
    }
};

// =============================================================================
// Test: Initial State
// =============================================================================

TEST_F(RouteControllerTest, InitialState) {
    EXPECT_EQ(controller->getState(), RouteController::State::NORMAL);

    auto decision = controller->getCurrentDecision();
    EXPECT_EQ(decision.video, RouteController::VideoRoute::USB);
    EXPECT_EQ(decision.control, RouteController::ControlRoute::USB);
    EXPECT_EQ(decision.main_fps, 60);
    EXPECT_EQ(decision.sub_fps, 30);
}

// =============================================================================
// Test: Device Registration
// =============================================================================

TEST_F(RouteControllerTest, RegisterDevice) {
    controller->registerDevice("device1", true, 5000);
    controller->registerDevice("device2", false, 5001);

    // Should still be in normal state
    EXPECT_EQ(controller->getState(), RouteController::State::NORMAL);
}

TEST_F(RouteControllerTest, SetMainDevice) {
    controller->registerDevice("device1", false, 5000);
    controller->registerDevice("device2", false, 5001);

    // Clear any registration commands
    fps_commands.clear();

    // Set device2 as main
    controller->setMainDevice("device2");

    // Evaluate to trigger FPS updates
    controller->evaluate(healthyUsb(), healthyWifi());

    // Main device should get higher FPS
    EXPECT_EQ(controller->getState(), RouteController::State::NORMAL);
}

// =============================================================================
// Test: Normal Operation
// =============================================================================

TEST_F(RouteControllerTest, NormalOperation) {
    controller->registerDevice("device1", true, 5000);

    auto decision = controller->evaluate(healthyUsb(), healthyWifi());

    EXPECT_EQ(decision.state, RouteController::State::NORMAL);
    EXPECT_EQ(decision.video, RouteController::VideoRoute::USB);
    EXPECT_EQ(decision.main_fps, 60);
}

// =============================================================================
// Test: USB Congestion Triggers State Change
// =============================================================================

TEST_F(RouteControllerTest, UsbCongestionTriggersOffload) {
    controller->registerDevice("device1", true, 5000);

    // Simulate 3+ seconds of USB congestion (threshold)
    for (int i = 0; i < 4; ++i) {
        controller->evaluate(congestedUsb(), healthyWifi());
    }

    // Should transition to offload or FPS reduced
    auto state = controller->getState();
    EXPECT_TRUE(state == RouteController::State::USB_OFFLOAD ||
                state == RouteController::State::FPS_REDUCED);
}

// =============================================================================
// Test: USB Failure Triggers Failover
// =============================================================================

TEST_F(RouteControllerTest, UsbFailureTriggersFailover) {
    controller->registerDevice("device1", true, 5000);

    // Simulate 5+ seconds of USB failure (threshold)
    for (int i = 0; i < 6; ++i) {
        controller->evaluate(deadUsb(), healthyWifi());
    }

    auto state = controller->getState();
    EXPECT_EQ(state, RouteController::State::USB_FAILED);
}

// =============================================================================
// Test: WiFi Failure
// =============================================================================

TEST_F(RouteControllerTest, WifiFailure) {
    controller->registerDevice("device1", true, 5000);

    // Start with USB offload state
    controller->forceState(RouteController::State::USB_OFFLOAD);

    // Simulate WiFi failure
    for (int i = 0; i < 6; ++i) {
        controller->evaluate(healthyUsb(), deadWifi());
    }

    // Should fall back to USB with reduced FPS
    auto state = controller->getState();
    EXPECT_TRUE(state == RouteController::State::WIFI_FAILED ||
                state == RouteController::State::NORMAL ||
                state == RouteController::State::FPS_REDUCED);
}

// =============================================================================
// Test: Recovery to Normal
// =============================================================================

TEST_F(RouteControllerTest, RecoveryToNormal) {
    controller->registerDevice("device1", true, 5000);

    // Force degraded state
    controller->forceState(RouteController::State::FPS_REDUCED);

    // Simulate sustained recovery
    for (int i = 0; i < 6; ++i) {
        controller->evaluate(healthyUsb(), healthyWifi());
    }

    // Should recover towards normal (may go through intermediate states)
    auto state = controller->getState();
    EXPECT_TRUE(state == RouteController::State::NORMAL ||
                state == RouteController::State::USB_OFFLOAD);
}

// =============================================================================
// Test: Reset to Normal
// =============================================================================

TEST_F(RouteControllerTest, ResetToNormal) {
    controller->registerDevice("device1", true, 5000);

    // Force failed state
    controller->forceState(RouteController::State::USB_FAILED);
    EXPECT_EQ(controller->getState(), RouteController::State::USB_FAILED);

    // Reset
    controller->resetToNormal();
    EXPECT_EQ(controller->getState(), RouteController::State::NORMAL);
}

// =============================================================================
// Test: Both Paths Degraded
// =============================================================================

TEST_F(RouteControllerTest, BothPathsDegraded) {
    controller->registerDevice("device1", true, 5000);

    // Simulate both USB and WiFi having issues
    for (int i = 0; i < 6; ++i) {
        controller->evaluate(congestedUsb(), deadWifi());
    }

    auto state = controller->getState();
    // Should be in some degraded state (USB_OFFLOAD happens first on congestion)
    EXPECT_TRUE(state == RouteController::State::BOTH_DEGRADED ||
                state == RouteController::State::WIFI_FAILED ||
                state == RouteController::State::FPS_REDUCED ||
                state == RouteController::State::USB_OFFLOAD);
}

// =============================================================================
// Test: FPS Decision Values
// =============================================================================

TEST_F(RouteControllerTest, FpsDecisionValues) {
    controller->registerDevice("main_device", true, 5000);
    controller->registerDevice("sub_device", false, 5001);

    auto decision = controller->evaluate(healthyUsb(), healthyWifi());

    // In normal state: main=60, sub=30
    EXPECT_EQ(decision.main_fps, 60);
    EXPECT_EQ(decision.sub_fps, 30);
}

// =============================================================================
// Test: Unregister Device
// =============================================================================

TEST_F(RouteControllerTest, UnregisterDevice) {
    controller->registerDevice("device1", true, 5000);
    controller->registerDevice("device2", false, 5001);

    controller->unregisterDevice("device1");

    // Should still work with remaining device
    auto decision = controller->evaluate(healthyUsb(), healthyWifi());
    EXPECT_EQ(decision.state, RouteController::State::NORMAL);
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
