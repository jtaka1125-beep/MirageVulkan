#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
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

        // Assigned port for screen capture (each device gets unique port)
        int assigned_port = 0;
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
    bool startScreenCapture(const std::string& adb_id, const std::string& host, int port);

    // Start screen capture on all unique devices (each gets base_port + index)
    int startScreenCaptureOnAll(const std::string& host, int base_port);

    // Assign ports to all devices starting from base_port
    void assignPorts(int base_port);

    // Get assigned port for device by hardware_id
    int getAssignedPort(const std::string& hardware_id) const;

    // Get device by assigned port
    bool getDeviceByPort(int port, UniqueDevice& out) const;

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
};

} // namespace gui
