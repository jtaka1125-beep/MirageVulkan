#pragma once
#include "mirror_receiver.hpp"
#include "adb_device_manager.hpp"
#include <memory>
#include <map>
#include <mutex>
#include <functional>

namespace gui {

/**
 * Multi-Device Video Receiver
 *
 * Manages multiple MirrorReceiver instances, one per device.
 * Each device sends video on its assigned port.
 *
 * Usage:
 *   1. Set AdbDeviceManager reference
 *   2. Call start(base_port) - creates receivers for each device
 *   3. Call get_latest_frame(hardware_id, frame) to get per-device frames
 */
class MultiDeviceReceiver {
public:
    struct DeviceStats {
        std::string hardware_id;
        std::string display_name;
        int port = 0;
        uint64_t packets = 0;
        uint64_t bytes = 0;
        float fps = 0.0f;
        float bandwidth_mbps = 0.0f;
        bool receiving = false;
        uint64_t last_frame_time = 0;
    };

    using FrameCallback = std::function<void(const std::string& hardware_id, const MirrorFrame& frame)>;

    MultiDeviceReceiver();
    ~MultiDeviceReceiver();

    // Set device manager (required before start)
    void setDeviceManager(AdbDeviceManager* manager) { adb_manager_ = manager; }

    // Start receivers for all devices (base_port, base_port+1, ...)
    bool start(uint16_t base_port = 60000);
    void stop();

    bool running() const { return running_; }

    // Get frame for specific device
    bool get_latest_frame(const std::string& hardware_id, MirrorFrame& out);

    // Get frame for device by port
    bool get_latest_frame_by_port(int port, MirrorFrame& out);

    // Get all device stats
    std::vector<DeviceStats> getStats() const;

    // Get active device count (devices receiving frames)
    int getActiveDeviceCount() const;

    // Frame callback (called when new frame received for any device)
    void setFrameCallback(FrameCallback cb) { frame_callback_ = std::move(cb); }

    // Get all hardware IDs of managed devices
    std::vector<std::string> getDeviceIds() const;
    int getPortForDevice(const std::string& hardware_id) const;

    // Feed RTP packet to the first device's receiver (for USB video)
    void feed_rtp_packet(const uint8_t* data, size_t len);

    // Get first device's hardware_id (for USB video)
    std::string getFirstDeviceId() const;

private:
    struct ReceiverEntry {
        std::unique_ptr<MirrorReceiver> receiver;
        std::string hardware_id;
        std::string display_name;
        int port = 0;

        // Stats
        uint64_t packets = 0;
        uint64_t bytes = 0;
        uint64_t frames = 0;
        float fps = 0.0f;
        float bandwidth_mbps = 0.0f;
        std::chrono::steady_clock::time_point last_frame_time;
        std::chrono::steady_clock::time_point last_stats_time;
        uint64_t prev_frames = 0;
        uint64_t prev_bytes = 0;
    };

    bool running_ = false;
    AdbDeviceManager* adb_manager_ = nullptr;

    mutable std::mutex receivers_mutex_;
    std::map<std::string, ReceiverEntry> receivers_;  // hardware_id -> receiver
    std::map<int, std::string> port_to_device_;       // port -> hardware_id

    FrameCallback frame_callback_;

    void updateStats();
};

} // namespace gui
