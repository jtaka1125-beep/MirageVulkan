#pragma once

#include "bandwidth_monitor.hpp"
#include <string>
#include <functional>
#include <map>
#include <chrono>

namespace gui {

/**
 * Controls video/control routing decisions based on bandwidth and health.
 *
 * Priority order when under pressure:
 * 1. Offload video to WiFi (keep control on USB)
 * 2. Reduce FPS gradually
 * 3. Failover to surviving path
 */
class RouteController {
public:
    enum class VideoRoute { USB, WIFI };
    enum class ControlRoute { USB, WIFI_ADB };

    enum class State {
        NORMAL,           // USB video+control, full FPS
        USB_OFFLOAD,      // Video on WiFi, control on USB
        FPS_REDUCED,      // FPS reduced due to congestion
        USB_FAILED,       // USB dead, all on WiFi
        WIFI_FAILED,      // WiFi dead, all on USB + FPS reduced
        BOTH_DEGRADED     // Both paths unstable
    };

    struct RouteDecision {
        VideoRoute video = VideoRoute::USB;
        ControlRoute control = ControlRoute::USB;
        int main_fps = 60;     // Target FPS for main device (USB)
        int sub_fps = 30;      // Target FPS for sub devices
        State state = State::NORMAL;
    };

    // Callback types
    using FpsCommandCallback = std::function<void(const std::string& device_id, int fps)>;
    using RouteCommandCallback = std::function<void(const std::string& device_id,
                                                     VideoRoute route,
                                                     const std::string& host,
                                                     int port)>;

    RouteController();

    // Set callbacks for sending commands
    void setFpsCommandCallback(FpsCommandCallback cb) { fps_callback_ = cb; }
    void setRouteCommandCallback(RouteCommandCallback cb) { route_callback_ = cb; }

    // Set main device (updates FPS targets)
    void setMainDevice(const std::string& device_id);

    // Register a device for routing
    void registerDevice(const std::string& device_id, bool is_main, int wifi_port);
    void unregisterDevice(const std::string& device_id);

    // Called periodically (e.g., every second) to evaluate and apply routing
    RouteDecision evaluate(const BandwidthMonitor::UsbStats& usb,
                           const BandwidthMonitor::WifiStats& wifi);

    // Get current state
    State getState() const { return state_; }
    RouteDecision getCurrentDecision() const { return current_; }

    // TCP-only mode (no USB video, using MirageCapture VID0)
    void setTcpOnlyMode(bool enabled) { tcp_only_mode_ = enabled; }
    bool isTcpOnlyMode() const { return tcp_only_mode_; }

    // Manual override (for testing)
    void forceState(State state);
    void resetToNormal();

private:
    void applyDecision(const RouteDecision& decision);
    int adjustFps(int current, int target, int step);

    struct DeviceInfo {
        std::string device_id;
        bool is_main = false;
        int wifi_port = 0;
        int current_fps = 30;
        VideoRoute current_video_route = VideoRoute::USB;
    };

    std::map<std::string, DeviceInfo> devices_;
    RouteDecision current_;
    State state_ = State::NORMAL;
    bool tcp_only_mode_ = false;

    // Consecutive counts for hysteresis
    int consecutive_usb_congestion_ = 0;
    int consecutive_usb_failure_ = 0;
    int consecutive_wifi_failure_ = 0;
    int consecutive_recovery_ = 0;

    // Thresholds for state transitions
    static constexpr int CONGESTION_THRESHOLD = 3;   // 3 seconds of congestion
    static constexpr int FAILURE_THRESHOLD = 5;      // 5 seconds of failure
    static constexpr int RECOVERY_THRESHOLD = 5;     // 5 seconds of recovery

    // FPS levels
    static constexpr int MAIN_FPS_HIGH = 60;
    static constexpr int MAIN_FPS_MED = 30;
    static constexpr int MAIN_FPS_LOW = 15;
    static constexpr int SUB_FPS_HIGH = 30;
    static constexpr int SUB_FPS_MED = 15;
    static constexpr int SUB_FPS_LOW = 10;

    // Throttled debug log
    std::chrono::steady_clock::time_point last_debug_log_{};

    FpsCommandCallback fps_callback_;
    RouteCommandCallback route_callback_;
    std::string wifi_host_;  // Set from config via setWifiHost()
};

} // namespace gui
