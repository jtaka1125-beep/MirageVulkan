#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <memory>
#include "auto_setup.hpp"
#include <fstream>

namespace gui {

/**
 * ADB Device Manager
 * Detects devices via ADB and identifies unique devices by hardware ID.
 * Handles duplicate detection when same device appears via USB and WiFi.
 */
class AdbDeviceManager {
public:
    enum class ConnectionType {
        USB,      // USB connection (serial number)
        WiFi,     // WiFi connection (IP:port)
        Unknown
    };

    struct DeviceInfo {
        std::string adb_id;           // ADB identifier (serial or IP:port)
        std::string hardware_id;      // Unique hardware ID (Android ID or serial)
        std::string model;            // Device model name
        std::string manufacturer;     // Manufacturer
        ConnectionType conn_type;     // USB or WiFi
        std::string ip_address;       // IP address (for WiFi or for USB with IP)
        bool is_online = false;

        // --- ディスプレイ・OS ---
        int screen_width = 0;         // 物理解像度 幅 (例: 800)
        int screen_height = 0;        // 物理解像度 高さ (例: 1340)
        int screen_density = 0;       // DPI (例: 240)
        std::string android_version;  // "15" (ro.build.version.release)
        int sdk_level = 0;            // 35 (ro.build.version.sdk)

        // For duplicate detection
        std::string unique_key() const { return hardware_id.empty() ? adb_id : hardware_id; }
    };

    struct UniqueDevice {
        std::string hardware_id;
        std::string display_name;
        std::string model;

        // All ADB connections to this device
        std::vector<std::string> usb_connections;   // USB serial numbers
        std::vector<std::string> wifi_connections;  // IP:port connections

        // Preferred connection (USB preferred over WiFi)
        std::string preferred_adb_id;
        ConnectionType preferred_type;
        std::string ip_address;

        // --- ディスプレイ・OS ---
        int screen_width = 0;
        int screen_height = 0;
        int screen_density = 0;
        std::string android_version;
        int sdk_level = 0;

        std::string usb_serial;         // USB physical serial (e.g. "A9250700479")

        // Assigned port for screen capture (each device gets unique port)
        int assigned_port = 0;
        int assigned_tcp_port = 0;  // scrcpy TCP port (from AutoSetup)
    };

    AdbDeviceManager() = default;
    ~AdbDeviceManager() = default;

    // Refresh device list (call periodically)
    void refresh();

    // Get unique devices (deduplicated)
    std::vector<UniqueDevice> getUniqueDevices() const;

    // Get all raw ADB devices
    std::vector<DeviceInfo> getAllDevices() const;

    // Get device info by ADB ID
    bool getDeviceInfo(const std::string& adb_id, DeviceInfo& out) const;

    // Get unique device by hardware ID
    bool getUniqueDevice(const std::string& hardware_id, UniqueDevice& out) const;

    // Install APK to all unique devices
    int installApkToAll(const std::string& apk_path);

    // Start app on all unique devices
    int startAppOnAll(const std::string& package_name, const std::string& activity);

    // Execute ADB command on specific device
    std::string adbCommand(const std::string& adb_id, const std::string& command);

    // Start screen capture on device with auto permission approval
    bool startScreenCapture(const std::string& adb_id, const std::string& host, int port, bool is_main = true);

    // Start screen capture on all unique devices (each gets base_port + index)
    int startScreenCaptureOnAll(const std::string& host, int base_port);

    // Assign ports to all devices starting from base_port
    void assignPorts(int base_port);

    // Get assigned port for device by hardware_id
    int getAssignedPort(const std::string& hardware_id) const;
    void setDevicePort(const std::string& hardware_id, int port);

    // Get device by assigned port
    bool getDeviceByPort(int port, UniqueDevice& out) const;

    // Resolve USB serial to hardware_id (for USB AOA → ADB device matching)
    std::string resolveUsbSerial(const std::string& usb_serial);

    // Send tap command to device
    void sendTap(const std::string& adb_id, int x, int y);

    // Send swipe command to device
    void sendSwipe(const std::string& adb_id, int x1, int y1, int x2, int y2, int duration_ms);

    // Send key event to device
    void sendKey(const std::string& adb_id, int keycode);

    // Take screenshot and return as raw PNG data
    std::vector<uint8_t> takeScreenshot(const std::string& adb_id);

    // Take screenshot, save to file, and optionally delete after use
    bool takeScreenshotToFile(const std::string& adb_id, const std::string& output_path);

    // Delete file from device
    bool deleteFile(const std::string& adb_id, const std::string& remote_path);

    // --- ディスプレイ・OS情報取得 ---
    // ADB経由で解像度・OS情報を取得してDeviceInfo/UniqueDeviceに格納
    void queryScreenInfo(const std::string& adb_id);

private:
    // Parse `adb devices` output
    std::vector<std::string> parseAdbDevices();

    // Get hardware ID from device (Android ID)
    std::string getHardwareId(const std::string& adb_id);

    // Get device properties
    std::string getDeviceProp(const std::string& adb_id, const std::string& prop);

    // Determine connection type from ADB ID
    ConnectionType determineConnectionType(const std::string& adb_id);

    // Extract IP from WiFi ADB ID
    std::string extractIp(const std::string& adb_id);

    mutable std::mutex mutex_;
    std::map<std::string, DeviceInfo> devices_;           // adb_id -> DeviceInfo
    std::map<std::string, UniqueDevice> unique_devices_;  // hardware_id -> UniqueDevice
    std::map<std::string, std::shared_ptr<mirage::AutoSetup>> active_setups_;  // adb_id -> persistent AutoSetup
};

} // namespace gui
