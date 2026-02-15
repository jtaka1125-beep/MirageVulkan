#pragma once
#include <string>
#include <vector>

namespace mirage {

/**
 * WinUSB Driver Checker
 * 
 * Checks if Android USB devices have WinUSB driver installed.
 * Used during startup and USB connection failure recovery to diagnose
 * driver issues and guide users to install WinUSB automatically.
 */
class WinUsbChecker {
public:
    struct UsbDeviceStatus {
        std::string vid;
        std::string pid;
        std::string name;
        std::string instance_id;
        std::string current_driver;  // "WinUSB", "usbccgp", "None", etc.
        bool needs_winusb = false;
    };

    /**
     * Check all connected Android USB devices for WinUSB driver status.
     * Uses PowerShell to query PnP device info.
     * @return Vector of device statuses
     */
    static std::vector<UsbDeviceStatus> checkDevices();

    /**
     * Parse raw pipe-delimited output into device statuses.
     * Format per line: "VID|PID|FriendlyName|InstanceId|Service"
     * Testable without PowerShell dependency.
     * @param raw_output Output from PowerShell enumeration
     * @return Vector of Android device statuses (non-Android VIDs filtered out)
     */
    static std::vector<UsbDeviceStatus> parseDeviceOutput(const std::string& raw_output);

    /**
     * Build diagnostic summary from pre-parsed device list.
     * @param devices Pre-parsed device list
     * @return Summary string
     */
    static std::string buildDiagnosticSummary(const std::vector<UsbDeviceStatus>& devices);

    /**
     * Quick check: are there any Android devices needing WinUSB?
     * @return true if at least one device needs WinUSB installed
     */
    static bool anyDeviceNeedsWinUsb();

    /**
     * Get a human-readable diagnostic summary (calls checkDevices internally).
     * @return Summary string (e.g., "2 devices OK, 1 needs WinUSB")
     */
    static std::string getDiagnosticSummary();

    /**
     * Launch the WinUSB installer script with admin elevation.
     * @param script_path Path to install_android_winusb.py
     * @return true if launch succeeded
     */
    static bool launchInstaller(const std::string& script_path);

    /**
     * Check if a VID belongs to a known Android device manufacturer.
     * @param vid 4-char hex VID string (e.g., "18D1")
     * @return true if recognized as Android VID
     */
    static bool isAndroidVid(const std::string& vid);
};

} // namespace mirage
