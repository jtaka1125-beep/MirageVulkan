#include "device_registry.hpp"
#include "mirage_log.hpp"
#include <cstdio>
#include <algorithm>

namespace gui {

// =============================================================================
// 端末登録・検索
// =============================================================================

DeviceEntity& DeviceRegistry::registerOrUpdate(const std::string& hardware_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = devices_.find(hardware_id);
    if (it != devices_.end()) {
        return it->second;  // 既存
    }

    // 新規登録
    auto& dev = devices_[hardware_id];
    dev.hardware_id = hardware_id;
    MLOG_INFO("Registry", "New device: %s", hardware_id.c_str());
    return dev;
}

DeviceEntity* DeviceRegistry::findByHardwareId(const std::string& hw_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    return (it != devices_.end()) ? &it->second : nullptr;
}

const DeviceEntity* DeviceRegistry::findByHardwareId(const std::string& hw_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    return (it != devices_.end()) ? &it->second : nullptr;
}

DeviceEntity* DeviceRegistry::findByUsbSerial(const std::string& serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = usb_serial_map_.find(serial);
    if (it != usb_serial_map_.end()) {
        auto dit = devices_.find(it->second);
        if (dit != devices_.end()) return &dit->second;
    }

    // Fallback: search all devices
    for (auto& [hw_id, dev] : devices_) {
        if (dev.usb_serial == serial) {
            usb_serial_map_[serial] = hw_id;  // Cache
            return &dev;
        }
        // Check if ADB USB ID contains serial
        if (!dev.adb_usb_id.empty() && dev.adb_usb_id.find(serial) != std::string::npos) {
            usb_serial_map_[serial] = hw_id;
            return &dev;
        }
    }
    return nullptr;
}

DeviceEntity* DeviceRegistry::findByAdbId(const std::string& adb_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = adb_id_map_.find(adb_id);
    if (it != adb_id_map_.end()) {
        auto dit = devices_.find(it->second);
        if (dit != devices_.end()) return &dit->second;
    }
    return nullptr;
}

DeviceEntity* DeviceRegistry::findByPort(int video_port) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = port_map_.find(video_port);
    if (it != port_map_.end()) {
        auto dit = devices_.find(it->second);
        if (dit != devices_.end()) return &dit->second;
    }
    return nullptr;
}

std::vector<DeviceEntity> DeviceRegistry::getAllDevices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceEntity> result;
    result.reserve(devices_.size());
    for (const auto& [hw_id, dev] : devices_) {
        result.push_back(dev);
    }
    return result;
}

std::vector<std::string> DeviceRegistry::getAllHardwareIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(devices_.size());
    for (const auto& [hw_id, dev] : devices_) {
        ids.push_back(hw_id);
    }
    return ids;
}

size_t DeviceRegistry::deviceCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_.size();
}

// =============================================================================
// 接続情報更新
// =============================================================================

void DeviceRegistry::setAdbUsb(const std::string& hw_id, const std::string& adb_id, const std::string& usb_serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    if (it == devices_.end()) return;

    it->second.adb_usb_id = adb_id;
    adb_id_map_[adb_id] = hw_id;

    if (!usb_serial.empty()) {
        it->second.usb_serial = usb_serial;
        usb_serial_map_[usb_serial] = hw_id;
    }

    // Extract serial from mDNS: "adb-SERIAL-hash._adb-tls-connect._tcp"
    if (usb_serial.empty() && adb_id.find("adb-") == 0) {
        size_t dash2 = adb_id.find('-', 4);
        if (dash2 != std::string::npos) {
            std::string extracted = adb_id.substr(4, dash2 - 4);
            it->second.usb_serial = extracted;
            usb_serial_map_[extracted] = hw_id;
        }
    }

    if (it->second.status == DeviceEntity::Status::Disconnected) {
        it->second.status = DeviceEntity::Status::AdbOnly;
    }

    notify(hw_id, "adb_usb");
}

void DeviceRegistry::setAdbWifi(const std::string& hw_id, const std::string& adb_id, const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    if (it == devices_.end()) return;

    it->second.adb_wifi_id = adb_id;
    adb_id_map_[adb_id] = hw_id;

    if (!ip.empty()) {
        it->second.ip_address = ip;
    }

    if (it->second.status == DeviceEntity::Status::Disconnected) {
        it->second.status = DeviceEntity::Status::AdbOnly;
    }

    notify(hw_id, "adb_wifi");
}

void DeviceRegistry::setAoaConnected(const std::string& hw_id, bool connected) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    if (it == devices_.end()) return;

    it->second.aoa_connected = connected;
    if (connected && it->second.status < DeviceEntity::Status::AoaActive) {
        it->second.status = DeviceEntity::Status::AoaActive;
    }

    notify(hw_id, "aoa");
}

void DeviceRegistry::setVideoPort(const std::string& hw_id, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    if (it == devices_.end()) return;

    // Remove old port mapping
    if (it->second.video_port != 0) {
        port_map_.erase(it->second.video_port);
    }

    it->second.video_port = port;
    port_map_[port] = hw_id;

    notify(hw_id, "video_port");
}

// =============================================================================
// 状態変更
// =============================================================================

void DeviceRegistry::setVideoRoute(const std::string& hw_id, DeviceEntity::VideoRoute route) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    if (it == devices_.end()) return;
    it->second.video_route = route;
    notify(hw_id, "video_route");
}

void DeviceRegistry::setControlRoute(const std::string& hw_id, DeviceEntity::ControlRoute route) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    if (it == devices_.end()) return;
    it->second.control_route = route;
    notify(hw_id, "control_route");
}

void DeviceRegistry::setTargetFps(const std::string& hw_id, int fps) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    if (it == devices_.end()) return;
    it->second.target_fps = fps;
    notify(hw_id, "target_fps");
}

void DeviceRegistry::setMainDevice(const std::string& hw_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Clear old main
    for (auto& [id, dev] : devices_) {
        dev.is_main = false;
    }

    auto it = devices_.find(hw_id);
    if (it != devices_.end()) {
        it->second.is_main = true;
        it->second.target_fps = 60;  // メインは60fps
    }

    main_device_id_ = hw_id;
    notify(hw_id, "main_device");
}

void DeviceRegistry::setStatus(const std::string& hw_id, DeviceEntity::Status status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    if (it == devices_.end()) return;
    it->second.status = status;
    notify(hw_id, "status");
}

void DeviceRegistry::updateStats(const std::string& hw_id, float fps, float bandwidth) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(hw_id);
    if (it == devices_.end()) return;
    it->second.current_fps = fps;
    it->second.bandwidth_mbps = bandwidth;
    // No notification for stats (too frequent)
}

std::string DeviceRegistry::getMainDeviceId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return main_device_id_;
}

// =============================================================================
// 変更通知
// =============================================================================

void DeviceRegistry::setChangeCallback(ChangeCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    change_cb_ = std::move(cb);
}

void DeviceRegistry::notify(const std::string& hw_id, const std::string& field) {
    // Note: called with mutex held, callback should be quick
    if (change_cb_) {
        change_cb_(hw_id, field);
    }
}

// =============================================================================
// デバッグ
// =============================================================================

void DeviceRegistry::dump() const {
    std::lock_guard<std::mutex> lock(mutex_);

    MLOG_INFO("Registry", "=== DeviceRegistry: %zu devices ===", devices_.size());
    for (const auto& [hw_id, dev] : devices_) {
        MLOG_INFO("Registry", "  [%s] %s", hw_id.c_str(), dev.display_name.c_str());
        MLOG_INFO("Registry", "    ADB USB: %s  WiFi: %s",
                dev.adb_usb_id.empty() ? "(none)" : dev.adb_usb_id.c_str(),
                dev.adb_wifi_id.empty() ? "(none)" : dev.adb_wifi_id.c_str());
        MLOG_INFO("Registry", "    USB serial: %s  IP: %s",
                dev.usb_serial.empty() ? "(none)" : dev.usb_serial.c_str(),
                dev.ip_address.empty() ? "(none)" : dev.ip_address.c_str());
        MLOG_INFO("Registry", "    AOA: %s  Port: %d  Route: %s",
                dev.aoa_connected ? "YES" : "no", dev.video_port,
                dev.video_route == DeviceEntity::VideoRoute::USB ? "USB" : "WiFi");
        MLOG_INFO("Registry", "    FPS: %.1f/%d  BW: %.1f Mbps  Main: %s",
                dev.current_fps, dev.target_fps, dev.bandwidth_mbps,
                dev.is_main ? "YES" : "no");
    }
    MLOG_INFO("Registry", "=================================");
}

} // namespace gui
