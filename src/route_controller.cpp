#include "route_controller.hpp"
#include "mirage_log.hpp"
#include "config_loader.hpp"

namespace gui {

RouteController::RouteController() {
    wifi_host_ = mirage::config::getConfig().network.pc_ip;
    current_.video = VideoRoute::USB;
    current_.control = ControlRoute::USB;
    current_.main_fps = MAIN_FPS_HIGH;  // 60 FPS for main device
    current_.sub_fps = SUB_FPS_HIGH;    // 30 FPS for sub devices
    current_.state = State::NORMAL;
}

void RouteController::registerDevice(const std::string& device_id, bool is_main, int wifi_port) {
    DeviceInfo info;
    info.device_id = device_id;
    info.is_main = is_main;
    info.wifi_port = wifi_port;
    info.current_fps = is_main ? MAIN_FPS_HIGH : SUB_FPS_HIGH;
    info.current_video_route = VideoRoute::USB;
    devices_[device_id] = info;

    MLOG_INFO("RouteCtrl", "Registered device %s (main=%d, wifi_port=%d)",
              device_id.c_str(), is_main, wifi_port);
}

void RouteController::unregisterDevice(const std::string& device_id) {
    devices_.erase(device_id);
    MLOG_INFO("RouteCtrl", "Unregistered device %s", device_id.c_str());
}

RouteController::RouteDecision RouteController::evaluate(
    const BandwidthMonitor::UsbStats& usb,
    const BandwidthMonitor::WifiStats& wifi)
{
    RouteDecision decision = current_;

    // Track consecutive states for hysteresis
    if (usb.is_congested) {
        consecutive_usb_congestion_++;
        consecutive_recovery_ = 0;
    } else {
        consecutive_usb_congestion_ = 0;
    }

    if (!usb.is_alive) {
        consecutive_usb_failure_++;
        consecutive_recovery_ = 0;
    } else {
        consecutive_usb_failure_ = 0;
    }

    if (!wifi.is_alive) {
        consecutive_wifi_failure_++;
        consecutive_recovery_ = 0;
    } else {
        consecutive_wifi_failure_ = 0;
    }

    // State machine
    switch (state_) {
    case State::NORMAL:
        if (consecutive_usb_failure_ >= FAILURE_THRESHOLD) {
            // USB died
            state_ = State::USB_FAILED;
            decision.video = VideoRoute::WIFI;
            decision.control = ControlRoute::WIFI_ADB;
            decision.main_fps = MAIN_FPS_MED;
            decision.sub_fps = SUB_FPS_MED;
            MLOG_INFO("RouteCtrl", "USB FAILED -> fallback to WiFi");
        } else if (consecutive_wifi_failure_ >= FAILURE_THRESHOLD) {
            // WiFi died
            state_ = State::WIFI_FAILED;
            decision.video = VideoRoute::USB;
            decision.control = ControlRoute::USB;
            decision.main_fps = MAIN_FPS_LOW;
            decision.sub_fps = SUB_FPS_LOW;
            MLOG_INFO("RouteCtrl", "WiFi FAILED -> USB only + FPS reduced");
        } else if (consecutive_usb_congestion_ >= CONGESTION_THRESHOLD) {
            // USB congested -> offload video to WiFi
            state_ = State::USB_OFFLOAD;
            decision.video = VideoRoute::WIFI;
            decision.control = ControlRoute::USB;  // Keep control on USB
            MLOG_INFO("RouteCtrl", "USB congested -> offload video to WiFi");
        }
        break;

    case State::USB_OFFLOAD:
        if (consecutive_usb_failure_ >= FAILURE_THRESHOLD) {
            state_ = State::USB_FAILED;
            decision.control = ControlRoute::WIFI_ADB;
            MLOG_INFO("RouteCtrl", "USB died while offloaded -> full WiFi");
        } else if (!usb.is_congested && !wifi.is_alive) {
            // WiFi died while offloaded, USB recovered
            state_ = State::WIFI_FAILED;
            decision.video = VideoRoute::USB;
            decision.main_fps = MAIN_FPS_LOW;
            decision.sub_fps = SUB_FPS_LOW;
            MLOG_INFO("RouteCtrl", "WiFi died -> back to USB + FPS reduced");
        } else if (wifi.packet_loss_rate > 0.1f) {
            // WiFi has high packet loss -> reduce FPS
            state_ = State::FPS_REDUCED;
            decision.main_fps = adjustFps(decision.main_fps, MAIN_FPS_LOW, -5);
            decision.sub_fps = adjustFps(decision.sub_fps, SUB_FPS_LOW, -5);
            MLOG_INFO("RouteCtrl", "WiFi packet loss -> reduce FPS");
        } else if (!usb.is_congested) {
            consecutive_recovery_++;
            if (consecutive_recovery_ >= RECOVERY_THRESHOLD) {
                state_ = State::NORMAL;
                decision.video = VideoRoute::USB;
                consecutive_recovery_ = 0;
                MLOG_INFO("RouteCtrl", "USB recovered -> back to normal");
            }
        }
        break;

    case State::FPS_REDUCED:
        if (!usb.is_congested && wifi.packet_loss_rate < 0.05f) {
            consecutive_recovery_++;
            if (consecutive_recovery_ >= RECOVERY_THRESHOLD) {
                // Gradually increase FPS
                decision.main_fps = adjustFps(decision.main_fps, MAIN_FPS_HIGH, 5);
                decision.sub_fps = adjustFps(decision.sub_fps, SUB_FPS_HIGH, 5);

                if (decision.main_fps >= MAIN_FPS_HIGH) {
                    state_ = State::USB_OFFLOAD;
                    MLOG_INFO("RouteCtrl", "FPS recovered -> USB_OFFLOAD");
                }
                consecutive_recovery_ = 0;
            }
        } else {
            consecutive_recovery_ = 0;
            // Continue reducing if needed
            if (wifi.packet_loss_rate > 0.15f) {
                decision.main_fps = adjustFps(decision.main_fps, MAIN_FPS_LOW, -5);
                decision.sub_fps = adjustFps(decision.sub_fps, SUB_FPS_LOW, -5);
            }
        }
        break;

    case State::USB_FAILED:
        if (usb.is_alive) {
            consecutive_recovery_++;
            if (consecutive_recovery_ >= RECOVERY_THRESHOLD) {
                state_ = State::USB_OFFLOAD;
                decision.control = ControlRoute::USB;
                consecutive_recovery_ = 0;
                MLOG_INFO("RouteCtrl", "USB recovered -> USB_OFFLOAD");
            }
        } else {
            consecutive_recovery_ = 0;
        }
        break;

    case State::WIFI_FAILED:
        if (wifi.is_alive) {
            consecutive_recovery_++;
            if (consecutive_recovery_ >= RECOVERY_THRESHOLD) {
                // Gradually increase FPS
                decision.main_fps = adjustFps(decision.main_fps, MAIN_FPS_HIGH, 5);
                decision.sub_fps = adjustFps(decision.sub_fps, SUB_FPS_HIGH, 5);

                if (decision.main_fps >= MAIN_FPS_HIGH) {
                    state_ = State::NORMAL;
                    MLOG_INFO("RouteCtrl", "WiFi recovered -> NORMAL");
                }
                consecutive_recovery_ = 0;
            }
        } else {
            consecutive_recovery_ = 0;
        }
        break;

    case State::BOTH_DEGRADED:
        // Try to recover to whichever path is alive
        if (usb.is_alive && !usb.is_congested) {
            state_ = State::WIFI_FAILED;
            decision.video = VideoRoute::USB;
            decision.control = ControlRoute::USB;
            MLOG_INFO("RouteCtrl", "USB recovered from degraded");
        } else if (wifi.is_alive) {
            state_ = State::USB_FAILED;
            decision.video = VideoRoute::WIFI;
            decision.control = ControlRoute::WIFI_ADB;
            MLOG_INFO("RouteCtrl", "WiFi recovered from degraded");
        }
        break;
    }

    decision.state = state_;

    // Apply if changed
    if (decision.video != current_.video ||
        decision.main_fps != current_.main_fps ||
        decision.sub_fps != current_.sub_fps) {
        applyDecision(decision);
    }

    current_ = decision;
    return decision;
}

void RouteController::applyDecision(const RouteDecision& decision) {
    for (auto& [id, info] : devices_) {
        // Apply FPS change
        int target_fps = info.is_main ? decision.main_fps : decision.sub_fps;
        if (target_fps != info.current_fps && fps_callback_) {
            fps_callback_(id, target_fps);
            info.current_fps = target_fps;
            MLOG_INFO("RouteCtrl", "Send FPS %d to %s", target_fps, id.c_str());
        }

        // Apply route change
        if (decision.video != info.current_video_route && route_callback_) {
            route_callback_(id, decision.video, wifi_host_, info.wifi_port);
            info.current_video_route = decision.video;
            MLOG_INFO("RouteCtrl", "Send route %s to %s",
                      decision.video == VideoRoute::WIFI ? "WiFi" : "USB", id.c_str());
        }
    }
}

int RouteController::adjustFps(int current, int target, int step) {
    if (step > 0) {
        return std::min(current + step, target);
    } else {
        return std::max(current + step, target);
    }
}

void RouteController::forceState(State state) {
    state_ = state;
    MLOG_INFO("RouteCtrl", "Force state: %d", static_cast<int>(state));
}

void RouteController::resetToNormal() {
    state_ = State::NORMAL;
    current_.video = VideoRoute::USB;
    current_.control = ControlRoute::USB;
    current_.main_fps = MAIN_FPS_HIGH;
    current_.sub_fps = SUB_FPS_HIGH;
    current_.state = State::NORMAL;

    consecutive_usb_congestion_ = 0;
    consecutive_usb_failure_ = 0;
    consecutive_wifi_failure_ = 0;
    consecutive_recovery_ = 0;

    for (auto& [id, info] : devices_) {
        info.current_fps = info.is_main ? MAIN_FPS_HIGH : SUB_FPS_HIGH;
        info.current_video_route = VideoRoute::USB;
    }

    MLOG_INFO("RouteCtrl", "Reset to normal");
}

void RouteController::setMainDevice(const std::string& device_id) {
    bool found = false;
    for (auto& [id, info] : devices_) {
        bool was_main = info.is_main;
        info.is_main = (id == device_id);

        if (info.is_main != was_main) {
            // FPS target changed
            int new_fps = info.is_main ? current_.main_fps : current_.sub_fps;
            if (info.current_fps != new_fps) {
                info.current_fps = new_fps;
                if (fps_callback_) {
                    fps_callback_(id, new_fps);
                }
                MLOG_INFO("RouteCtrl", "%s: %s -> %d fps",
                          id.c_str(), info.is_main ? "MAIN" : "sub", new_fps);
            }
        }

        if (info.is_main) found = true;
    }

    if (!found) {
        MLOG_WARN("RouteCtrl", "setMainDevice: %s not registered", device_id.c_str());
    }
}

} // namespace gui
