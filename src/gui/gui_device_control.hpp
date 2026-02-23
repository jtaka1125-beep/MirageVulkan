// =============================================================================
// MirageSystem v2 GUI - Device Control
// =============================================================================
// AOA mode switching and ADB connection management
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace mirage::gui::device_control {

// =============================================================================
// AOA Mode Control
// =============================================================================

// Switch ALL devices to AOA mode (called once at startup)
// Returns: number of devices successfully switched
int switchAllDevicesToAOA();

// Switch specific device to AOA mode
// Returns: true if successful
bool switchDeviceToAOA(const std::string& device_id);

// Check if device is in AOA mode
bool isDeviceInAOAMode(const std::string& device_id);

// =============================================================================
// ADB Connection Control
// =============================================================================

// Connect device via ADB (USB or WiFi)
// Returns: true if successful
bool connectDeviceADB(const std::string& device_id);

// Disconnect device ADB
bool disconnectDeviceADB(const std::string& device_id);

// Check if device has ADB connection
bool hasADBConnection(const std::string& device_id);

// Get ADB connection type ("usb", "wifi", "none")
std::string getADBConnectionType(const std::string& device_id);

// =============================================================================
// Device Info
// =============================================================================

struct DeviceControlInfo {
    std::string device_id;
    std::string display_name;
    bool in_aoa_mode;
    bool has_adb;
    std::string adb_type;  // "usb", "wifi", "none"
    std::string ip_address;
    // Device details (from AdbDeviceManager::UniqueDevice)
    int screen_width = 0;
    int screen_height = 0;
    std::string android_version;
    int sdk_level = 0;
    int battery_level = -1;  // -1 = unknown
};

// Get control info for all devices
std::vector<DeviceControlInfo> getAllDeviceControlInfo();

// Get control info for specific device
DeviceControlInfo getDeviceControlInfo(const std::string& device_id);

// =============================================================================
// GUI Rendering
// =============================================================================

// Render the "Switch All to AOA" button (for toolbar)
// Returns: true if button was clicked
bool renderSwitchAllAOAButton();

// Render individual device ADB button
// Returns: true if button was clicked
bool renderDeviceADBButton(const std::string& device_id);

// Render device control panel (full panel with all controls)
void renderDeviceControlPanel();

} // namespace mirage::gui::device_control
