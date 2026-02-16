#include "multi_device_receiver.hpp"
#include <cstdio>
#include "mirage_log.hpp"

namespace gui {

MultiDeviceReceiver::MultiDeviceReceiver() = default;

MultiDeviceReceiver::~MultiDeviceReceiver() {
    stop();
}

bool MultiDeviceReceiver::start(uint16_t base_port) {
    if (running_) return true;
    if (!adb_manager_) {
        MLOG_INFO("multi", "No device manager set");
        return false;
    }

    auto devices = adb_manager_->getUniqueDevices();
    if (devices.empty()) {
        MLOG_INFO("multi", "No devices found");
        return false;
    }

    std::lock_guard<std::mutex> lock(receivers_mutex_);

    for (const auto& device : devices) {
        ReceiverEntry entry;
        entry.hardware_id = device.hardware_id;
        entry.display_name = device.display_name;
        entry.receiver = std::make_unique<MirrorReceiver>();
        entry.last_stats_time = std::chrono::steady_clock::now();

        // Use port=0 for OS-assigned port (or base_port if specified)
        uint16_t request_port = (base_port == 0) ? 0 : base_port++;
        if (entry.receiver->start(request_port)) {
            // Get actual bound port from receiver
            entry.port = entry.receiver->getPort();
            MLOG_INFO("multi", "Started receiver for %s on port %d", device.display_name.c_str(), entry.port);

            port_to_device_[entry.port] = device.hardware_id;
            receivers_[device.hardware_id] = std::move(entry);
        } else {
            MLOG_ERROR("multi", "Failed to start receiver for %s", device.display_name.c_str());
        }
    }

    running_ = !receivers_.empty();
    MLOG_INFO("multi", "Started %zu receivers", receivers_.size());
    return running_;
}

void MultiDeviceReceiver::stop() {
    if (!running_) return;

    std::lock_guard<std::mutex> lock(receivers_mutex_);

    for (auto& [hw_id, entry] : receivers_) {
        if (entry.receiver) {
            entry.receiver->stop();
        }
    }

    receivers_.clear();
    port_to_device_.clear();
    running_ = false;

    MLOG_INFO("multi", "Stopped all receivers");
}

bool MultiDeviceReceiver::get_latest_frame(const std::string& hardware_id, MirrorFrame& out) {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    auto it = receivers_.find(hardware_id);
    if (it == receivers_.end()) return false;

    auto& entry = it->second;
    if (!entry.receiver) return false;

    if (entry.receiver->get_latest_frame(out)) {
        entry.frames++;
        entry.last_frame_time = std::chrono::steady_clock::now();

        // Update stats periodically
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - entry.last_stats_time).count();

        if (elapsed >= 1000) {
            float elapsed_sec = elapsed / 1000.0f;
            entry.fps = (entry.frames - entry.prev_frames) / elapsed_sec;

            uint64_t current_bytes = entry.receiver->bytes_received();
            uint64_t new_bytes = current_bytes - entry.prev_bytes;
            entry.bandwidth_mbps = (new_bytes * 8.0f / 1000000.0f) / elapsed_sec;
            entry.bytes = current_bytes;
            entry.packets = entry.receiver->packets_received();

            entry.prev_frames = entry.frames;
            entry.prev_bytes = current_bytes;
            entry.last_stats_time = now;
        }

        // Call frame callback if set
        if (frame_callback_) {
            frame_callback_(hardware_id, out);
        }

        return true;
    }

    return false;
}

bool MultiDeviceReceiver::get_latest_frame_by_port(int port, MirrorFrame& out) {
    // Get receiver pointer with minimal lock time
    MirrorReceiver* receiver_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(receivers_mutex_);

        auto it = port_to_device_.find(port);
        if (it == port_to_device_.end()) return false;

        const std::string& hw_id = it->second;
        auto entry_it = receivers_.find(hw_id);
        if (entry_it == receivers_.end()) return false;

        auto& entry = entry_it->second;
        receiver_ptr = entry.receiver.get();
    }
    // Lock released - receiver access is safe because we own unique_ptr

    if (!receiver_ptr) return false;
    return receiver_ptr->get_latest_frame(out);
}

std::vector<MultiDeviceReceiver::DeviceStats> MultiDeviceReceiver::getStats() const {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    std::vector<DeviceStats> stats;
    stats.reserve(receivers_.size());

    auto now = std::chrono::steady_clock::now();

    for (const auto& [hw_id, entry] : receivers_) {
        DeviceStats ds;
        ds.hardware_id = entry.hardware_id;
        ds.display_name = entry.display_name;
        ds.port = entry.port;
        ds.packets = entry.packets;
        ds.bytes = entry.bytes;
        ds.fps = entry.fps;
        ds.bandwidth_mbps = entry.bandwidth_mbps;

        // Check if receiving (had frame in last 2 seconds)
        auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - entry.last_frame_time).count();
        ds.receiving = (since_last < 2000);
        ds.last_frame_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.last_frame_time.time_since_epoch()).count();

        stats.push_back(ds);
    }

    return stats;
}

int MultiDeviceReceiver::getActiveDeviceCount() const {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    auto now = std::chrono::steady_clock::now();
    int count = 0;

    for (const auto& [hw_id, entry] : receivers_) {
        auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - entry.last_frame_time).count();
        if (since_last < 2000) {
            count++;
        }
    }

    return count;
}

std::vector<std::string> MultiDeviceReceiver::getDeviceIds() const {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    std::vector<std::string> ids;
    ids.reserve(receivers_.size());

    for (const auto& [hw_id, entry] : receivers_) {
        ids.push_back(hw_id);
    }

    return ids;
}

void MultiDeviceReceiver::feed_rtp_packet(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    // Feed to first device's receiver (USB video from primary device)
    if (!receivers_.empty()) {
        auto& entry = receivers_.begin()->second;
        if (entry.receiver) {
            entry.receiver->feed_rtp_packet(data, len);
        }
    }
}

std::string MultiDeviceReceiver::getFirstDeviceId() const {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    if (!receivers_.empty()) {
        return receivers_.begin()->first;
    }
    return "";
}

} // namespace gui
