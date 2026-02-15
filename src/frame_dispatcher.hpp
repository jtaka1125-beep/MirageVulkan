// =============================================================================
// MirageSystem - Frame Dispatcher
// =============================================================================
// Service layer between receivers and GUI.
// Auto-registers new devices, publishes FrameReadyEvent via EventBus.
// =============================================================================
#pragma once
#include "event_bus.hpp"
#include "mirage_log.hpp"
#include <string>
#include <mutex>
#include <set>

namespace mirage {

class FrameDispatcher {
public:
    void dispatchFrame(const std::string& device_id,
                       const uint8_t* rgba_data, int width, int height,
                       uint64_t frame_id = 0) {
        // Auto-register new devices
        {
            std::lock_guard<std::mutex> lock(devices_mutex_);
            if (known_devices_.find(device_id) == known_devices_.end()) {
                known_devices_.insert(device_id);

                DeviceConnectedEvent evt;
                evt.device_id = device_id;
                evt.display_name = device_id;
                evt.connection_type = "auto";
                bus().publish(evt);

                MLOG_INFO("dispatch", "Auto-registered device: %s", device_id.c_str());
            }
        }

        FrameReadyEvent evt;
        evt.device_id = device_id;
        evt.rgba_data = rgba_data;
        evt.width = width;
        evt.height = height;
        evt.frame_id = frame_id;
        bus().publish(evt);
    }

    void dispatchStatus(const std::string& device_id, int status,
                        float fps = 0, float latency_ms = 0, float bandwidth_mbps = 0) {
        DeviceStatusEvent evt;
        evt.device_id = device_id;
        evt.status = status;
        evt.fps = fps;
        evt.latency_ms = latency_ms;
        evt.bandwidth_mbps = bandwidth_mbps;
        bus().publish(evt);
    }

    void dispatchDisconnect(const std::string& device_id) {
        {
            std::lock_guard<std::mutex> lock(devices_mutex_);
            known_devices_.erase(device_id);
        }
        DeviceDisconnectedEvent evt;
        evt.device_id = device_id;
        bus().publish(evt);
    }

    void registerDevice(const std::string& device_id, const std::string& display_name,
                        const std::string& conn_type = "usb") {
        {
            std::lock_guard<std::mutex> lock(devices_mutex_);
            if (known_devices_.count(device_id)) return; // already registered
            known_devices_.insert(device_id);
        }
        DeviceConnectedEvent evt;
        evt.device_id = device_id;
        evt.display_name = display_name;
        evt.connection_type = conn_type;
        bus().publish(evt);
        MLOG_INFO("dispatch", "Registered device: %s (%s)", device_id.c_str(), display_name.c_str());
    }

    bool isKnownDevice(const std::string& device_id) const {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        return known_devices_.count(device_id) > 0;
    }

private:
    mutable std::mutex devices_mutex_;
    std::set<std::string> known_devices_;
};

// Global dispatcher singleton
inline FrameDispatcher& dispatcher() {
    static FrameDispatcher instance;
    return instance;
}

} // namespace mirage
