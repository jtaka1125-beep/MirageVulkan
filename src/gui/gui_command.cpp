// =============================================================================
// MirageSystem v2 GUI - Command Functions
// =============================================================================
// Tap, Swipe, and other device commands
// =============================================================================
#include "mirage_log.hpp"
#include "event_bus.hpp"
#include "gui_command.hpp"
#include "gui_state.hpp"
#include <cstdio>

namespace mirage::gui::command {

using namespace mirage::gui::state;

// =============================================================================
// EventBus購読ハンドル（RAII）
// =============================================================================

static mirage::SubscriptionHandle s_tap_sub;
static mirage::SubscriptionHandle s_swipe_sub;
static mirage::SubscriptionHandle s_key_sub;

static const char* sourceStr(CommandSource src) {
    switch (src) {
        case CommandSource::AI:    return "AI";
        case CommandSource::USER:  return "USER";
        case CommandSource::MACRO: return "MACRO";
        default:                   return "?";
    }
}

void init() {
    // TapCommandEvent購読 → sendTapCommand()
    s_tap_sub = mirage::bus().subscribe<TapCommandEvent>(
        [](const TapCommandEvent& evt) {
            MLOG_INFO("cmd", "EventBus TapCommand: device=%s (%d,%d) source=%s",
                      evt.device_id.c_str(), evt.x, evt.y, sourceStr(evt.source));
            sendTapCommand(evt.device_id, evt.x, evt.y);
        });

    // SwipeCommandEvent購読 → sendSwipeCommand()
    s_swipe_sub = mirage::bus().subscribe<SwipeCommandEvent>(
        [](const SwipeCommandEvent& evt) {
            MLOG_INFO("cmd", "EventBus SwipeCommand: device=%s (%d,%d)->(%d,%d) dur=%dms source=%s",
                      evt.device_id.c_str(), evt.x1, evt.y1, evt.x2, evt.y2,
                      evt.duration_ms, sourceStr(evt.source));
            sendSwipeCommand(evt.device_id, evt.x1, evt.y1, evt.x2, evt.y2, evt.duration_ms);
        });

    // KeyCommandEvent購読 → sendKeyCommand()
    s_key_sub = mirage::bus().subscribe<KeyCommandEvent>(
        [](const KeyCommandEvent& evt) {
            MLOG_INFO("cmd", "EventBus KeyCommand: device=%s key=%d source=%s",
                      evt.device_id.c_str(), evt.keycode, sourceStr(evt.source));
            sendKeyCommand(evt.device_id, evt.keycode);
        });

    MLOG_INFO("cmd", "EventBus コマンド購読開始 (Tap/Swipe/Key)");
}

void shutdown() {
    // SubscriptionHandle代入でRAII解除
    s_tap_sub = mirage::SubscriptionHandle();
    s_swipe_sub = mirage::SubscriptionHandle();
    s_key_sub = mirage::SubscriptionHandle();
    MLOG_INFO("cmd", "EventBus コマンド購読解除");
}

// =============================================================================
// ID Resolution: hardware_id -> USB serial for AOA commands
// =============================================================================
// GUI uses hardware_id (android_id hash) but HybridCommandSender uses USB serial.
// This resolves the mismatch.

static std::string resolveToUsbId(const std::string& device_id) {
    if (!g_hybrid_cmd) return device_id;

    // Check if already a USB serial (directly connected)
    if (g_hybrid_cmd->is_device_connected(device_id)) {
        return device_id;
    }

    // Try resolving via ADB manager: hardware_id -> usb_connections[0]
    if (g_adb_manager) {
        auto devices = g_adb_manager->getUniqueDevices();
        for (const auto& dev : devices) {
            if (dev.hardware_id == device_id && !dev.usb_connections.empty()) {
                const auto& usb_serial = dev.usb_connections[0];
                // Verify this USB serial is actually connected via AOA
                if (g_hybrid_cmd->is_device_connected(usb_serial)) {
                    MLOG_INFO("cmd", "Resolved %s -> %s (USB AOA)", device_id.c_str(), usb_serial.c_str());
                    return usb_serial;
                }
            }
        }
    }

    // Also try matching against hybrid_cmd's device list
    auto usb_ids = g_hybrid_cmd->get_device_ids();
    if (g_adb_manager) {
        for (const auto& usb_id : usb_ids) {
            // Check if this USB device maps to the requested hardware_id
            auto devices = g_adb_manager->getUniqueDevices();
            for (const auto& dev : devices) {
                if (dev.hardware_id == device_id) {
                    // Check USB connections
                    for (const auto& conn : dev.usb_connections) {
                        if (conn == usb_id) {
                            MLOG_INFO("cmd", "Resolved %s -> %s (USB conn match)", device_id.c_str(), usb_id.c_str());
                            return usb_id;
                        }
                    }
                    // Check mDNS-style ADB IDs (adb-SERIAL-xxx)
                    for (const auto& conn : dev.usb_connections) {
                        if (usb_id.find(conn) != std::string::npos || conn.find(usb_id) != std::string::npos) {
                            MLOG_INFO("cmd", "Resolved %s -> %s (partial match)", device_id.c_str(), usb_id.c_str());
                            return usb_id;
                        }
                    }
                }
            }
        }
    }

    MLOG_INFO("cmd", "Could not resolve %s to USB ID", device_id.c_str());
    return device_id;  // Return as-is, let caller handle

}


// Prefer USB ADB serial for low-latency input when falling back to adb shell input
static std::string resolvePreferredAdbIdForInput(const ::gui::AdbDeviceManager::UniqueDevice& dev) {
    if (!g_adb_manager) return dev.preferred_adb_id;

    // 1) Prefer any ONLINE USB serial
    for (const auto& usb : dev.usb_connections) {
        ::gui::AdbDeviceManager::DeviceInfo info;
        if (g_adb_manager->getDeviceInfo(usb, info) && info.is_online) {
            return usb;
        }
    }

    // 2) Fall back to preferred (may be WiFi)
    if (!dev.preferred_adb_id.empty()) {
        ::gui::AdbDeviceManager::DeviceInfo info;
        if (g_adb_manager->getDeviceInfo(dev.preferred_adb_id, info) && info.is_online) {
            return dev.preferred_adb_id;
        }
    }

    // 3) Any ONLINE WiFi entry
    for (const auto& w : dev.wifi_connections) {
        ::gui::AdbDeviceManager::DeviceInfo info;
        if (g_adb_manager->getDeviceInfo(w, info) && info.is_online) {
            return w;
        }
    }
    return dev.preferred_adb_id;
}



// =============================================================================
// Tap Commands
// =============================================================================

void sendTapCommandToAll(int x, int y) {
    if (g_hybrid_cmd && g_hybrid_cmd->usb_connected()) {
        int count = g_hybrid_cmd->send_tap_all(x, y);
        auto gui = g_gui;
        if (gui) {
            gui->logInfo(u8"USB タップ x" + std::to_string(count) +
                         " (" + std::to_string(x) + ", " + std::to_string(y) + ")");
        }
    }
}

void sendTapCommand(const std::string& device_id, int x, int y) {
    MLOG_INFO("cmd", "device='%s' coords=(%d,%d) hybrid_cmd=%s", device_id.c_str(), x, y, g_hybrid_cmd ? "yes" : "no");

    auto gui = g_gui;

    if (g_hybrid_cmd) {
        std::string usb_id = resolveToUsbId(device_id);
        bool connected = g_hybrid_cmd->is_device_connected(usb_id);
        MLOG_INFO("cmd", "resolved='%s' connected=%s", usb_id.c_str(), connected ? "yes" : "no");

        if (connected) {
            g_hybrid_cmd->send_tap(usb_id, x, y);
            MLOG_INFO("cmd", "USB tap sent to %s!", usb_id.c_str());
            if (gui) {
                gui->logInfo(u8"USB タップ " + device_id +
                             " (" + std::to_string(x) + ", " + std::to_string(y) + ")");
            }
            return;
        }
    }

    // ADB fallback: use adb shell input tap
    if (g_adb_manager) {
        auto devices = g_adb_manager->getUniqueDevices();
        for (const auto& dev : devices) {
            if (dev.hardware_id == device_id) {
                std::string cmd = "shell input tap " + std::to_string(x) + " " + std::to_string(y);
                std::string adb_id = resolvePreferredAdbIdForInput(dev);
                g_adb_manager->adbCommand(adb_id, cmd);
                MLOG_INFO("cmd", "ADB tap sent to %s via %s", device_id.c_str(), adb_id.c_str());
                if (gui) {
                    gui->logInfo(u8"ADB タップ " + dev.display_name +
                                 " (" + std::to_string(x) + ", " + std::to_string(y) + ")");
                }
                return;
            }
        }
    }

    // Fall back to IPC
    MLOG_INFO("cmd", "Falling back to IPC");
    if (!g_ipc) {
        MLOG_ERROR("cmd", "No USB/ADB/IPC path available for %s", device_id.c_str());
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), R"({"type":"tap","slot":0,"x":%d,"y":%d})", x, y);
    g_ipc->request_once(cmd);

    if (gui) {
        gui->logInfo(u8"IPC タップ (" + std::to_string(x) + ", " + std::to_string(y) + ")");
    }
}

// =============================================================================
// Swipe Commands
// =============================================================================

void sendSwipeCommandToAll(int x1, int y1, int x2, int y2, int duration_ms) {
    if (g_hybrid_cmd && g_hybrid_cmd->usb_connected()) {
        int count = g_hybrid_cmd->send_swipe_all(x1, y1, x2, y2, duration_ms);
        auto gui = g_gui;
        if (gui) {
            gui->logInfo(u8"USB スワイプ x" + std::to_string(count) +
                         " (" + std::to_string(x1) + "," + std::to_string(y1) +
                         ") -> (" + std::to_string(x2) + "," + std::to_string(y2) + ")");
        }
    }
}

void sendSwipeCommand(const std::string& device_id, int x1, int y1, int x2, int y2, int duration_ms) {
    MLOG_INFO("cmd", "device='%s' (%d,%d)->(%d,%d) dur=%dms hybrid_cmd=%s", device_id.c_str(), x1, y1, x2, y2, duration_ms, g_hybrid_cmd ? "yes" : "no");

    auto gui = g_gui;

    if (g_hybrid_cmd) {
        std::string usb_id = resolveToUsbId(device_id);
        bool connected = g_hybrid_cmd->is_device_connected(usb_id);
        MLOG_INFO("cmd", "resolved='%s' connected=%s", usb_id.c_str(), connected ? "yes" : "no");

        if (connected) {
            g_hybrid_cmd->send_swipe(usb_id, x1, y1, x2, y2, duration_ms);
            MLOG_INFO("cmd", "USB swipe sent to %s!", usb_id.c_str());
            if (gui) {
                gui->logInfo(u8"USB スワイプ " + device_id +
                             " (" + std::to_string(x1) + "," + std::to_string(y1) +
                             ") -> (" + std::to_string(x2) + "," + std::to_string(y2) + ")");
            }
            return;
        }
    }

    // ADB fallback
    if (g_adb_manager) {
        auto devices = g_adb_manager->getUniqueDevices();
        for (const auto& dev : devices) {
            if (dev.hardware_id == device_id) {
                std::string cmd = "shell input swipe " +
                    std::to_string(x1) + " " + std::to_string(y1) + " " +
                    std::to_string(x2) + " " + std::to_string(y2) + " " +
                    std::to_string(duration_ms);
                std::string adb_id = resolvePreferredAdbIdForInput(dev);
                g_adb_manager->adbCommand(adb_id, cmd);
                MLOG_INFO("cmd", "ADB swipe sent to %s", device_id.c_str());
                if (gui) {
                    gui->logInfo(u8"ADB スワイプ " + dev.display_name +
                                 " (" + std::to_string(x1) + "," + std::to_string(y1) +
                                 ") -> (" + std::to_string(x2) + "," + std::to_string(y2) + ")");
                }
                return;
            }
        }
    }

    // Fall back to IPC
    MLOG_INFO("cmd", "Falling back to IPC");
    if (!g_ipc) {
        MLOG_ERROR("cmd", "No USB/ADB/IPC path available for %s", device_id.c_str());
        return;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             R"({"type":"swipe","slot":0,"x1":%d,"y1":%d,"x2":%d,"y2":%d,"duration":%d})",
             x1, y1, x2, y2, duration_ms);
    g_ipc->request_once(cmd);

    if (gui) {
        gui->logInfo(u8"IPC スワイプ (" + std::to_string(x1) + "," + std::to_string(y1) +
                     ") -> (" + std::to_string(x2) + "," + std::to_string(y2) + ")");
    }
}

// =============================================================================
// Key Commands
// =============================================================================

void sendKeyCommand(const std::string& device_id, int keycode) {
    if (g_hybrid_cmd) {
        std::string usb_id = resolveToUsbId(device_id);
        if (g_hybrid_cmd->is_device_connected(usb_id)) {
            g_hybrid_cmd->send_key(usb_id, keycode);
            MLOG_INFO("cmd", "USB key %d sent to %s", keycode, usb_id.c_str());
            return;
        }
    }

    // ADB fallback
    if (g_adb_manager) {
        auto devices = g_adb_manager->getUniqueDevices();
        for (const auto& dev : devices) {
            if (dev.hardware_id == device_id) {
                std::string cmd = "shell input keyevent " + std::to_string(keycode);
                std::string adb_id = resolvePreferredAdbIdForInput(dev);
                g_adb_manager->adbCommand(adb_id, cmd);
                MLOG_INFO("cmd", "ADB key %d sent to %s", keycode, device_id.c_str());
                return;
            }
        }
    }
}

// =============================================================================
// Helper Functions
// =============================================================================

std::string getDeviceIdFromSlot(int slot) {
    if (!g_hybrid_cmd) return "";

    auto device_ids = g_hybrid_cmd->get_device_ids();
    if (slot >= 0 && slot < (int)device_ids.size()) {
        return device_ids[slot];
    }
    return "";
}

} // namespace mirage::gui::command
