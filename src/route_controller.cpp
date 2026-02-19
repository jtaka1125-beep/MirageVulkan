#include "route_controller.hpp"
#include "mirage_log.hpp"
#include "config_loader.hpp"

namespace gui {

static const char* stateToStr(RouteController::State s) {
    switch (s) {
    case RouteController::State::NORMAL: return "NORMAL";
    case RouteController::State::USB_OFFLOAD: return "USB_OFFLOAD";
    case RouteController::State::FPS_REDUCED: return "FPS_REDUCED";
    case RouteController::State::USB_FAILED: return "USB_FAILED";
    case RouteController::State::WIFI_FAILED: return "WIFI_FAILED";
    case RouteController::State::BOTH_DEGRADED: return "BOTH_DEGRADED";
    default: return "?";
    }
}

static const char* routeToStr(RouteController::VideoRoute r) {
    return r == RouteController::VideoRoute::WIFI ? "WIFI" : "USB";
}

RouteController::RouteController() {
    wifi_host_ = mirage::config::getConfig().network.pc_ip;
    current_.video = VideoRoute::USB;
    current_.control = ControlRoute::USB;
    current_.main_fps = MAIN_FPS_HIGH;  // 60 FPS for main device
    current_.sub_fps = SUB_FPS_HIGH;    // 30 FPS for sub devices
    current_.state = State::NORMAL;
    last_debug_log_ = std::chrono::steady_clock::now();
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

    const State prev_state = state_;
    const RouteDecision prev_decision = current_;
    const auto now = std::chrono::steady_clock::now();

    // Track consecutive states for hysteresis
    // TCP_ONLY: USB counters skipped (USB always dead, would reset recovery counter)
    if (!tcp_only_mode_) {
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
    }

    if (!wifi.is_alive) {
        consecutive_wifi_failure_++;
        consecutive_recovery_ = 0;
    } else {
        consecutive_wifi_failure_ = 0;
    }

    const bool wifi_failed = (consecutive_wifi_failure_ >= FAILURE_THRESHOLD);
    const bool usb_failed = (consecutive_usb_failure_ >= FAILURE_THRESHOLD);
    const bool usb_congested = (consecutive_usb_congestion_ >= CONGESTION_THRESHOLD);

    const float wifi_loss = wifi.packet_loss_rate;

    const auto log_tick = std::chrono::duration_cast<std::chrono::seconds>(now - last_debug_log_).count();
    if (log_tick >= 10) {
        last_debug_log_ = now;
        MLOG_INFO(
            "RouteCtrl",
            "tick: state=%s usb[bw=%.1f rtt=%.1f alive=%d cong=%d ccong=%d cfail=%d] wifi[bw=%.1f loss=%.2f alive=%d cfail=%d] decision[route=%s main=%d sub=%d]",
            stateToStr(state_),
            usb.bandwidth_mbps, usb.ping_rtt_ms, usb.is_alive ? 1 : 0, usb.is_congested ? 1 : 0,
            consecutive_usb_congestion_, consecutive_usb_failure_,
            wifi.bandwidth_mbps, wifi_loss, wifi.is_alive ? 1 : 0, consecutive_wifi_failure_,
            routeToStr(current_.video), current_.main_fps, current_.sub_fps
        );
    }

    // TCP-only mode: USB統計を無視し、WiFi統計のみでFPS制御
    if (tcp_only_mode_) {
        if (wifi_failed) {
            // WiFi死亡 = TCP-onlyでは全経路死亡、最小FPSへ
            decision.main_fps = MAIN_FPS_LOW;
            decision.sub_fps = SUB_FPS_LOW;
            state_ = State::BOTH_DEGRADED;
        } else if (wifi_loss > 0.10f) {
            // 高パケットロス - 積極的に削減
            decision.main_fps = adjustFps(current_.main_fps, MAIN_FPS_MED, -10);
            decision.sub_fps = adjustFps(current_.sub_fps, SUB_FPS_LOW, -5);
        } else if (wifi_loss > 0.05f) {
            // 中程度のロス - 段階的に削減
            decision.main_fps = adjustFps(current_.main_fps, MAIN_FPS_MED, -5);
            decision.sub_fps = adjustFps(current_.sub_fps, SUB_FPS_MED, -5);
        } else {
            // WiFi正常 - 最大値に向けて増加
            decision.main_fps = adjustFps(current_.main_fps, MAIN_FPS_HIGH, 5);
            decision.sub_fps = adjustFps(current_.sub_fps, SUB_FPS_HIGH, 5);
            if (state_ != State::NORMAL) {
                consecutive_recovery_++;
                if (consecutive_recovery_ >= RECOVERY_THRESHOLD) {
                    state_ = State::NORMAL;
                    consecutive_recovery_ = 0;
                    MLOG_INFO("RouteCtrl", "TCP_ONLY: recovered -> NORMAL (main=%d sub=%d)",
                              decision.main_fps, decision.sub_fps);
                }
            }
        }

        decision.video = VideoRoute::WIFI;
        decision.control = ControlRoute::WIFI_ADB;
        decision.state = state_;

        // 変更があれば適用
        if (decision.video != current_.video ||
            decision.main_fps != current_.main_fps ||
            decision.sub_fps != current_.sub_fps) {
            applyDecision(decision);
        }

        // 状態変化ログ
        if (prev_state != state_) {
            MLOG_INFO("RouteCtrl",
                "TCP_ONLY STATE %s -> %s | wifi(bw=%.1f loss=%.2f alive=%d) MainFPS=%d SubFPS=%d",
                stateToStr(prev_state), stateToStr(state_),
                wifi.bandwidth_mbps, wifi_loss, wifi.is_alive ? 1 : 0,
                decision.main_fps, decision.sub_fps);
        }

        // 定期デバッグログ
        if (log_tick >= 10) {
            MLOG_INFO("RouteEval", "TCP_ONLY: State=%s WiFi=%.1fMbps(loss=%.2f,alive=%d) MainFPS=%d SubFPS=%d",
                      stateToStr(state_), wifi.bandwidth_mbps, wifi_loss, wifi.is_alive ? 1 : 0,
                      decision.main_fps, decision.sub_fps);
        }

        current_ = decision;
        return decision;  // USB状態マシンをスキップ
    }

    // State machine
    switch (state_) {
    case State::NORMAL:
        if (usb_failed) {
            // USB died
            state_ = State::USB_FAILED;
            decision.video = VideoRoute::WIFI;
            decision.control = ControlRoute::WIFI_ADB;
            decision.main_fps = MAIN_FPS_MED;
            decision.sub_fps = SUB_FPS_MED;
            MLOG_INFO("RouteCtrl", "NORMAL -> USB_FAILED (usb dead)");
        } else if (wifi_failed) {
            // WiFi died
            state_ = State::WIFI_FAILED;
            decision.video = VideoRoute::USB;
            decision.control = ControlRoute::USB;
            decision.main_fps = MAIN_FPS_LOW;
            decision.sub_fps = SUB_FPS_LOW;
            MLOG_INFO("RouteCtrl", "NORMAL -> WIFI_FAILED (wifi dead)");
        } else if (usb_congested) {
            // USB congested -> offload video to WiFi
            state_ = State::USB_OFFLOAD;
            decision.video = VideoRoute::WIFI;
            decision.control = ControlRoute::USB;  // Keep control on USB
            MLOG_INFO("RouteCtrl", "NORMAL -> USB_OFFLOAD (usb congested)");
        }
        break;

    case State::USB_OFFLOAD:
        if (usb_failed) {
            state_ = State::USB_FAILED;
            decision.control = ControlRoute::WIFI_ADB;
            MLOG_INFO("RouteCtrl", "USB_OFFLOAD -> USB_FAILED (usb dead)");
        } else if (wifi_failed) {
            // WiFi died while offloaded -> back to USB + FPS reduced
            state_ = State::WIFI_FAILED;
            decision.video = VideoRoute::USB;
            decision.control = ControlRoute::USB;
            decision.main_fps = MAIN_FPS_LOW;
            decision.sub_fps = SUB_FPS_LOW;
            MLOG_INFO("RouteCtrl", "USB_OFFLOAD -> WIFI_FAILED (wifi dead)");
        } else if (wifi_loss > 0.10f) {
            // WiFi has high packet loss -> reduce FPS
            state_ = State::FPS_REDUCED;
            decision.main_fps = adjustFps(decision.main_fps, MAIN_FPS_LOW, -5);
            decision.sub_fps = adjustFps(decision.sub_fps, SUB_FPS_LOW, -5);
            MLOG_INFO("RouteCtrl", "USB_OFFLOAD -> FPS_REDUCED (wifi loss %.2f)", wifi_loss);
        } else if (!usb.is_congested) {
            consecutive_recovery_++;
            if (consecutive_recovery_ >= RECOVERY_THRESHOLD) {
                state_ = State::NORMAL;
                decision.video = VideoRoute::USB;
                consecutive_recovery_ = 0;
                MLOG_INFO("RouteCtrl", "USB_OFFLOAD -> NORMAL (usb recovered)");
            }
        }
        break;

    case State::FPS_REDUCED:
        if (!usb.is_congested && wifi_loss < 0.05f && !wifi_failed) {
            consecutive_recovery_++;
            if (consecutive_recovery_ >= RECOVERY_THRESHOLD) {
                // Gradually increase FPS
                decision.main_fps = adjustFps(decision.main_fps, MAIN_FPS_HIGH, 5);
                decision.sub_fps = adjustFps(decision.sub_fps, SUB_FPS_HIGH, 5);

                if (decision.main_fps >= MAIN_FPS_HIGH) {
                    state_ = State::USB_OFFLOAD;
                    MLOG_INFO("RouteCtrl", "FPS_REDUCED -> USB_OFFLOAD (fps recovered)");
                }
                consecutive_recovery_ = 0;
            }
        } else {
            consecutive_recovery_ = 0;
            // Continue reducing if needed
            if (wifi_loss > 0.15f) {
                decision.main_fps = adjustFps(decision.main_fps, MAIN_FPS_LOW, -5);
                decision.sub_fps = adjustFps(decision.sub_fps, SUB_FPS_LOW, -5);
            }
            if (wifi_failed) {
                state_ = State::WIFI_FAILED;
                decision.video = VideoRoute::USB;
                decision.control = ControlRoute::USB;
                decision.main_fps = MAIN_FPS_LOW;
                decision.sub_fps = SUB_FPS_LOW;
                MLOG_INFO("RouteCtrl", "FPS_REDUCED -> WIFI_FAILED (wifi dead)");
            }
            if (usb_failed) {
                state_ = State::USB_FAILED;
                decision.video = VideoRoute::WIFI;
                decision.control = ControlRoute::WIFI_ADB;
                decision.main_fps = MAIN_FPS_MED;
                decision.sub_fps = SUB_FPS_MED;
                MLOG_INFO("RouteCtrl", "FPS_REDUCED -> USB_FAILED (usb dead)");
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
                MLOG_INFO("RouteCtrl", "USB_FAILED -> USB_OFFLOAD (usb recovered)");
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
                    MLOG_INFO("RouteCtrl", "WIFI_FAILED -> NORMAL (wifi recovered)");
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
            MLOG_INFO("RouteCtrl", "BOTH_DEGRADED -> WIFI_FAILED (usb recovered)");
        } else if (wifi.is_alive) {
            state_ = State::USB_FAILED;
            decision.video = VideoRoute::WIFI;
            decision.control = ControlRoute::WIFI_ADB;
            MLOG_INFO("RouteCtrl", "BOTH_DEGRADED -> USB_FAILED (wifi recovered)");
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

    // Extra one-shot summary on state change
    if (prev_state != state_) {
        MLOG_INFO(
            "RouteCtrl",
            "STATE %s -> %s | usb(bw=%.1f rtt=%.1f alive=%d cong=%d) wifi(bw=%.1f loss=%.2f alive=%d)",
            stateToStr(prev_state), stateToStr(state_),
            usb.bandwidth_mbps, usb.ping_rtt_ms, usb.is_alive ? 1 : 0, usb.is_congested ? 1 : 0,
            wifi.bandwidth_mbps, wifi_loss, wifi.is_alive ? 1 : 0
        );
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
    MLOG_INFO("RouteCtrl", "Force state: %s", stateToStr(state_));
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
