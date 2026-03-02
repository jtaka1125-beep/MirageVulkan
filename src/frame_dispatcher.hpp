// =============================================================================
// MirageSystem - Frame Dispatcher
// =============================================================================
// Service layer between receivers and GUI.
// Auto-registers new devices, publishes FrameReadyEvent via EventBus.
// Now uses SharedFrame for zero-copy frame delivery.
// =============================================================================
#pragma once
#include "event_bus.hpp"
#include "mirage_log.hpp"
#include <string>
#include <mutex>
#include <set>
#include <vector>
#include <unordered_map>
#include <cstring>

namespace mirage {

class FrameDispatcher {
public:
    // SharedFrame-based dispatch (single copy, shared across all consumers)

    // Direct SharedFrame dispatch (no copy, for internal use)
    void dispatchSharedFrame(std::shared_ptr<SharedFrame> frame) {
        if (!frame) return;

        // Auto-register new devices
        {
            std::lock_guard<std::mutex> lock(devices_mutex_);
            if (known_devices_.find(frame->device_id) == known_devices_.end()) {
                known_devices_.insert(frame->device_id);

                DeviceConnectedEvent evt;
                evt.device_id = frame->device_id;
                evt.display_name = frame->device_id;
                evt.connection_type = "auto";
                bus().publish(evt);

                MLOG_INFO("dispatch", "Auto-registered device: %s", frame->device_id.c_str());
            }
        }

        FrameReadyEvent evt;
        evt.device_id = frame->device_id;
        evt.rgba_data = frame->data();
        evt.width = frame->width;
        evt.height = frame->height;
        evt.frame_id = frame->frame_id;
        evt.source_port = frame->source_port;
        evt.frame = frame;
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
