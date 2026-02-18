// =============================================================================
// MirageSystem - TCP Video Receiver (ADB Forward Mode)
// =============================================================================
#pragma once

#include "mirror_receiver.hpp"
#include "adb_device_manager.hpp"
#include <memory>
#include <map>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

namespace gui {

class TcpVideoReceiver {
public:
    TcpVideoReceiver();
    ~TcpVideoReceiver();

    void setDeviceManager(AdbDeviceManager* mgr);
    bool start(int base_port = 50100);
    void stop();
    bool running() const { return running_.load(); }
    std::vector<std::string> getDeviceIds() const;
    
    // Get latest decoded frame for a specific device
    bool get_latest_frame(const std::string& hardware_id, MirrorFrame& out);

private:
    struct DeviceEntry {
        std::string hardware_id;
        std::string adb_serial;
        int local_port = 0;
        std::unique_ptr<MirrorReceiver> decoder;
        std::thread thread;
        uint64_t pkt_count = 0;  // per-device packet counter
        bool header_skipped = false;  // scrcpy raw_stream codec header (12 bytes) skipped
    };

    void receiverThread(const std::string& hardware_id, const std::string& serial, int local_port);
    void parseVid0Stream(const std::string& hardware_id, std::vector<uint8_t>& buffer);
    void parseRawH264Stream(const std::string& hardware_id, std::vector<uint8_t>& buffer);
    bool setupAdbForward(const std::string& serial, int local_port);
    void removeAdbForward(const std::string& serial, int local_port);
    bool launchScrcpyServer(const std::string& serial, int local_port);

    AdbDeviceManager* adb_mgr_ = nullptr;
    mutable std::mutex devices_mutex_;
    std::map<std::string, DeviceEntry> devices_;
    std::atomic<bool> running_{false};
};

} // namespace gui
