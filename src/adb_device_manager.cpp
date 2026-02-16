#include "adb_device_manager.hpp"
#include "auto_setup.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <array>
#include <thread>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <cctype>
#include <regex>
#include <cstdlib>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#include <windows.h>
#include "mirage_log.hpp"
#endif

namespace gui {

// =============================================================================
// Security: Input validation and sanitization
// =============================================================================

namespace {

// RAII wrapper for FILE* with pclose
struct PipeDeleter {
    void operator()(FILE* fp) const {
        if (fp) pclose(fp);
    }
};
using UniquePipe = std::unique_ptr<FILE, PipeDeleter>;

// Execute command without showing console window (Windows)
// Returns command output as string, or empty on failure
std::string execCommandHidden(const std::string& cmd) {
    std::string result;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return result;
    }

    // Ensure read handle is not inherited
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = nullptr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmd_copy = "cmd /c " + cmd;

    if (CreateProcessA(nullptr, cmd_copy.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWritePipe);
        hWritePipe = nullptr;

        // Read output
        char buffer[4096];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            result += buffer;
        }

        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    if (hWritePipe) CloseHandle(hWritePipe);
    CloseHandle(hReadPipe);

    return result;
}

// Dangerous shell metacharacters that could enable command injection
constexpr const char* SHELL_METACHARACTERS = "|;&$`\\\"'<>(){}[]!#*?~\n\r";

/**
 * Validate ADB device ID format
 * Valid formats:
 *   - Serial number: alphanumeric, may include ':'
 *   - IP:port: xxx.xxx.xxx.xxx:port
 * Returns true if valid, false if potentially malicious
 */
bool isValidAdbId(const std::string& adb_id) {
    if (adb_id.empty() || adb_id.length() > 64) {
        return false;
    }

    // Check for shell metacharacters
    for (char c : adb_id) {
        if (std::strchr(SHELL_METACHARACTERS, c) != nullptr) {
            MLOG_ERROR("adb", "WARNING: Invalid character in device ID: '%c'", c);
            return false;
        }
    }

    // Must be alphanumeric, colons, dots, or hyphens
    for (char c : adb_id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != ':' && c != '.' && c != '-' && c != '_') {
            MLOG_WARN("adb", "WARNING: Unexpected character in device ID: '%c'", c);
            return false;
        }
    }

    return true;
}

/**
 * Sanitize command string for shell execution
 * Returns sanitized string or empty string if input is dangerous
 */
std::string sanitizeCommand(const std::string& command) {
    if (command.empty()) {
        return "";
    }

    // Check for extremely dangerous patterns
    static const std::regex dangerous_patterns(
        R"(\$\(|\`|;\s*rm|;\s*dd|>\s*/|<\s*/|\|\s*sh|\|\s*bash)",
        std::regex::icase
    );

    if (std::regex_search(command, dangerous_patterns)) {
        MLOG_WARN("adb", "WARNING: Potentially dangerous command blocked");
        return "";
    }

    return command;
}

// Note: escapeShellArg was removed as unused. If needed for future shell escaping,
// implement using platform-specific APIs (e.g., CommandLineToArgvW on Windows).

/**
 * Get platform-appropriate temporary directory
 */
std::string getTempDirectory() {
#ifdef _WIN32
    // Use GetTempPath on Windows
    char temp_path[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, temp_path);
    if (len > 0 && len < MAX_PATH) {
        return std::string(temp_path);
    }
    // Fallback to environment variable
    const char* tmp = std::getenv("TEMP");
    if (tmp) return std::string(tmp) + "\\";
    tmp = std::getenv("TMP");
    if (tmp) return std::string(tmp) + "\\";
    return "C:\\Temp\\";
#else
    // Unix-like systems
    const char* tmp = std::getenv("TMPDIR");
    if (tmp) return std::string(tmp) + "/";
    return "/tmp/";
#endif
}

} // anonymous namespace

// =============================================================================
// ADB Command Execution
// =============================================================================

std::string AdbDeviceManager::adbCommand(const std::string& adb_id, const std::string& command) {
    // Security validation
    if (!isValidAdbId(adb_id)) {
        MLOG_ERROR("adb", "ERROR: Invalid device ID rejected: %s", adb_id.c_str());
        return "";
    }

    std::string sanitized_cmd = sanitizeCommand(command);
    if (sanitized_cmd.empty() && !command.empty()) {
        MLOG_ERROR("adb", "ERROR: Command rejected by security filter");
        return "";
    }

    // Build command safely
    std::string cmd = "adb -s " + adb_id + " " + sanitized_cmd + " 2>&1";

    // Use hidden window execution to prevent console flash
    std::string result = execCommandHidden(cmd);

    // Prevent excessive output (DoS protection)
    if (result.size() > 1024 * 1024) {
        MLOG_WARN("adb", "WARNING: Output truncated (exceeded 1MB)");
        result.resize(1024 * 1024);
    }

    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }

    return result;
}

std::vector<std::string> AdbDeviceManager::parseAdbDevices() {
    std::vector<std::string> devices;

    std::string cmd = "adb devices 2>&1";

    // Use hidden window execution to prevent console flash
    std::string result = execCommandHidden(cmd);
    MLOG_INFO("adb", "Raw adb output (%zu bytes): [%s]", result.size(), result.substr(0, 500).c_str());
    if (result.empty()) {
        MLOG_ERROR("adb", "ERROR: Failed to execute 'adb devices'");
        return devices;
    }

    // Parse output line by line
    std::istringstream iss(result);
    std::string line;
    while (std::getline(iss, line)) {
        // Skip header and empty lines
        if (line.find("List of devices") != std::string::npos) continue;
        if (line.empty()) continue;

        // Parse "device_id\tdevice" or "device_id\toffline"
        size_t tab_pos = line.find('\t');
        if (tab_pos == std::string::npos) continue;

        std::string id = line.substr(0, tab_pos);
        std::string status = line.substr(tab_pos + 1);

        // Trim status
        while (!status.empty() && (status.back() == '\n' || status.back() == '\r' || status.back() == ' ')) {
            status.pop_back();
        }

        if (status == "device") {
            devices.push_back(id);
        }
    }

    return devices;
}

AdbDeviceManager::ConnectionType AdbDeviceManager::determineConnectionType(const std::string& adb_id) {
    // WiFi connection format: IP:PORT (e.g., "192.168.0.5:5555")
    if (adb_id.find(':') != std::string::npos) {
        // Check if it looks like IP:port
        size_t colon_pos = adb_id.find(':');
        std::string ip_part = adb_id.substr(0, colon_pos);

        // Simple IP check (contains dots)
        if (std::count(ip_part.begin(), ip_part.end(), '.') == 3) {
            return ConnectionType::WiFi;
        }
    }

    // Otherwise assume USB
    return ConnectionType::USB;
}

std::string AdbDeviceManager::extractIp(const std::string& adb_id) {
    size_t colon_pos = adb_id.find(':');
    if (colon_pos != std::string::npos) {
        return adb_id.substr(0, colon_pos);
    }
    return "";
}

std::string AdbDeviceManager::getDeviceProp(const std::string& adb_id, const std::string& prop) {
    return adbCommand(adb_id, "shell getprop " + prop);
}

std::string AdbDeviceManager::getHardwareId(const std::string& adb_id) {
    // Try Android ID first (unique per device)
    // Note: We hash the Android ID for privacy - never log the raw value
    std::string android_id = adbCommand(adb_id, "shell settings get secure android_id");
    if (!android_id.empty() && android_id.find("error") == std::string::npos) {
        // Return a truncated/hashed version for privacy (first 8 chars + hash suffix)
        // This is still unique enough for device identification but harder to trace
        if (android_id.length() > 8) {
            // Simple hash: polynomial rolling hash for a suffix
            // Using larger modulo (100000000) to reduce collision probability
            unsigned int hash = 0;
            for (char c : android_id) {
                hash = hash * 31 + static_cast<unsigned char>(c);
            }
            return android_id.substr(0, 8) + "_" + std::to_string(hash % 100000000);
        }
        return android_id;
    }

    // Fall back to serial number (less sensitive)
    std::string serial = getDeviceProp(adb_id, "ro.serialno");
    if (!serial.empty() && serial.find("error") == std::string::npos) {
        return serial;
    }

    // Last resort: use ADB ID itself
    return adb_id;
}

void AdbDeviceManager::refresh() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Clear old data
    devices_.clear();
    unique_devices_.clear();

    // Get all ADB devices
    auto adb_ids = parseAdbDevices();

    // Collect device info
    for (const auto& adb_id : adb_ids) {
        DeviceInfo info;
        info.adb_id = adb_id;
        info.conn_type = determineConnectionType(adb_id);
        info.is_online = true;

        // Get hardware ID
        info.hardware_id = getHardwareId(adb_id);

        // Get device model
        info.model = getDeviceProp(adb_id, "ro.product.model");
        info.manufacturer = getDeviceProp(adb_id, "ro.product.manufacturer");

        // Get IP address
        if (info.conn_type == ConnectionType::WiFi) {
            info.ip_address = extractIp(adb_id);
        } else {
            // For USB, try to get IP from wlan0
            std::string ip_output = adbCommand(adb_id, "shell ip addr show wlan0 | grep 'inet '");
            // Parse: "    inet 192.168.0.5/24 ..."
            size_t inet_pos = ip_output.find("inet ");
            if (inet_pos != std::string::npos) {
                size_t ip_start = inet_pos + 5;
                size_t ip_end = ip_output.find('/', ip_start);
                if (ip_end != std::string::npos) {
                    info.ip_address = ip_output.substr(ip_start, ip_end - ip_start);
                }
            }
        }

        devices_[adb_id] = info;

        // Group by hardware ID
        auto& unique = unique_devices_[info.hardware_id];
        unique.hardware_id = info.hardware_id;
        unique.model = info.model;
        unique.display_name = info.manufacturer + " " + info.model;

        if (info.conn_type == ConnectionType::USB) {
            unique.usb_connections.push_back(adb_id);
        } else {
            unique.wifi_connections.push_back(adb_id);
        }

        // Set IP if available
        if (!info.ip_address.empty()) {
            unique.ip_address = info.ip_address;
        }
    }

    // Determine preferred connection for each unique device
    for (auto& [hw_id, unique] : unique_devices_) {
        if (!unique.usb_connections.empty()) {
            // Prefer USB
            unique.preferred_adb_id = unique.usb_connections[0];
            unique.preferred_type = ConnectionType::USB;
        } else if (!unique.wifi_connections.empty()) {
            // Fall back to WiFi
            unique.preferred_adb_id = unique.wifi_connections[0];
            unique.preferred_type = ConnectionType::WiFi;
        }
    }

    MLOG_INFO("adb", "Found %zu devices (%zu unique)", devices_.size(), unique_devices_.size());
}

std::vector<AdbDeviceManager::UniqueDevice> AdbDeviceManager::getUniqueDevices() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<UniqueDevice> result;
    for (const auto& [hw_id, device] : unique_devices_) {
        result.push_back(device);
    }
    return result;
}

std::vector<AdbDeviceManager::DeviceInfo> AdbDeviceManager::getAllDevices() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<DeviceInfo> result;
    for (const auto& [adb_id, info] : devices_) {
        result.push_back(info);
    }
    return result;
}

bool AdbDeviceManager::getDeviceInfo(const std::string& adb_id, DeviceInfo& out) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = devices_.find(adb_id);
    if (it == devices_.end()) return false;

    out = it->second;
    return true;
}

bool AdbDeviceManager::getUniqueDevice(const std::string& hardware_id, UniqueDevice& out) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = unique_devices_.find(hardware_id);
    if (it == unique_devices_.end()) return false;

    out = it->second;
    return true;
}

int AdbDeviceManager::installApkToAll(const std::string& apk_path) {
    auto devices = getUniqueDevices();
    int success_count = 0;

    for (const auto& device : devices) {
        MLOG_INFO("adb", "Installing to %s (%s)...", device.display_name.c_str(), device.preferred_adb_id.c_str());

        std::string result = adbCommand(device.preferred_adb_id, "install -r \"" + apk_path + "\"");

        if (result.find("Success") != std::string::npos) {
            MLOG_INFO("adb", "Success");
            success_count++;
        } else {
            MLOG_ERROR("adb", "Failed: %s", result.c_str());
        }
    }

    return success_count;
}

int AdbDeviceManager::startAppOnAll(const std::string& package_name, const std::string& activity) {
    auto devices = getUniqueDevices();
    int success_count = 0;

    for (const auto& device : devices) {
        std::string cmd = "shell am start -n " + package_name + "/" + activity;
        std::string result = adbCommand(device.preferred_adb_id, cmd);

        if (result.find("Error") == std::string::npos) {
            success_count++;
        }
    }

    return success_count;
}

std::vector<uint8_t> AdbDeviceManager::takeScreenshot(const std::string& adb_id) {
    std::vector<uint8_t> result;

    // Take screenshot on device
    // Use /data/local/tmp/ for Android 13+ compatibility (Scoped Storage)
    std::string remote_path = "/data/local/tmp/mirage_screenshot.png";
    std::string cmd = "shell screencap -p " + remote_path;
    adbCommand(adb_id, cmd);

    // Pull to local temp file (platform-independent)
    std::string local_path = getTempDirectory() + "mirage_screenshot_" + adb_id + ".png";

    // Use exec-out to get raw data directly
    // Validate adb_id before using in shell command
    if (!isValidAdbId(adb_id)) {
        MLOG_ERROR("adb", "ERROR: Invalid device ID for screenshot");
        return result;
    }

    std::string pull_cmd = "adb -s " + adb_id + " exec-out cat " + remote_path;

    // Use RAII for pipe management
    UniquePipe pipe(popen(pull_cmd.c_str(), "rb"));
    if (pipe) {
        char buffer[4096];
        size_t bytes_read;
        constexpr size_t MAX_SCREENSHOT_SIZE = 50 * 1024 * 1024;  // 50MB limit

        while ((bytes_read = fread(buffer, 1, sizeof(buffer), pipe.get())) > 0) {
            result.insert(result.end(), buffer, buffer + bytes_read);

            // Size limit check
            if (result.size() > MAX_SCREENSHOT_SIZE) {
                MLOG_WARN("adb", "WARNING: Screenshot too large, truncating");
                break;
            }
        }
    } else {
        MLOG_ERROR("adb", "ERROR: Failed to execute screenshot pull command");
    }

    // Delete remote file (cleanup - failure is non-critical)
    if (!deleteFile(adb_id, remote_path)) {
        MLOG_ERROR("adb", "Warning: Failed to clean up remote file %s", remote_path.c_str());
    }

    MLOG_INFO("adb", "Screenshot captured: %zu bytes", result.size());
    return result;
}

bool AdbDeviceManager::takeScreenshotToFile(const std::string& adb_id, const std::string& output_path) {
    // Take screenshot on device
    // Use /data/local/tmp/ for Android 13+ compatibility (Scoped Storage)
    std::string remote_path = "/data/local/tmp/mirage_screenshot.png";
    std::string cmd = "shell screencap -p " + remote_path;
    adbCommand(adb_id, cmd);

    // Pull to local file
    std::string pull_cmd = "pull " + remote_path + " \"" + output_path + "\"";
    std::string result = adbCommand(adb_id, pull_cmd);

    // Delete remote file (cleanup - failure is non-critical)
    if (!deleteFile(adb_id, remote_path)) {
        MLOG_ERROR("adb", "Warning: Failed to clean up remote file %s", remote_path.c_str());
    }

    bool success = (result.find("error") == std::string::npos) &&
                   (result.find("Error") == std::string::npos);

    MLOG_INFO("adb", "Screenshot saved to %s: %s", output_path.c_str(), success ? "OK" : "FAILED");
    return success;
}

bool AdbDeviceManager::deleteFile(const std::string& adb_id, const std::string& remote_path) {
    // Security: Validate remote_path to prevent command injection
    // Only allow paths in /data/local/tmp/ and /sdcard/ directories
    if (remote_path.empty() ||
        (remote_path.find("/data/local/tmp/") != 0 && remote_path.find("/sdcard/") != 0)) {
        MLOG_WARN("adb", "WARNING: Refusing to delete path outside allowed directories: %s", remote_path.c_str());
        return false;
    }

    // Check for shell metacharacters
    for (char c : remote_path) {
        if (std::strchr(SHELL_METACHARACTERS, c) != nullptr) {
            MLOG_ERROR("adb", "WARNING: Invalid character in delete path");
            return false;
        }
    }

    std::string cmd = "shell rm -f " + remote_path;
    adbCommand(adb_id, cmd);
    return true;
}

bool AdbDeviceManager::startScreenCapture(const std::string& adb_id, const std::string& host, int port) {
    MLOG_INFO("adb", "Starting screen capture on %s -> %s:%d", adb_id.c_str(), host.c_str(), port);

    // Create persistent AutoSetup (must outlive this function for bridge thread)
    auto setup_ptr = std::make_shared<mirage::AutoSetup>();
    setup_ptr->set_adb_executor([this, adb_id](const std::string& cmd) -> std::string {
        return adbCommand(adb_id, cmd);
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_setups_[adb_id] = setup_ptr;  // Keep alive
    }
    auto& setup = *setup_ptr;

    // Start screen capture
    auto result1 = setup.start_screen_capture(host, port);
    if (result1.status != mirage::SetupStatus::COMPLETED) {
        MLOG_ERROR("adb", "Failed to start screen capture: %s", result1.message.c_str());
        return false;
    }

    // Wait for dialog to appear
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Approve screen share dialog
    auto result2 = setup.approve_screen_share_dialog();
    MLOG_INFO("adb", "Screen share dialog result: %s", result2.message.c_str());

    // Complete and verify - go to home screen
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto result3 = setup.complete_and_verify();
    MLOG_INFO("adb", "Complete and verify: %s", result3.message.c_str());

    return (result2.status == mirage::SetupStatus::COMPLETED ||
            result2.status == mirage::SetupStatus::SKIPPED) &&
           (result3.status == mirage::SetupStatus::COMPLETED);
}

int AdbDeviceManager::startScreenCaptureOnAll(const std::string& host, int base_port) {
    // Assign ports only if base_port > 0 (otherwise pre-assigned via setDevicePort)
    if (base_port > 0) {
        assignPorts(base_port);
    }

    auto devices = getUniqueDevices();
    int success_count = 0;

    for (const auto& device : devices) {
        int device_port = device.assigned_port;
        MLOG_INFO("adb", "Starting screen capture on %s (%s) -> %s:%d", device.display_name.c_str(), device.preferred_adb_id.c_str(),
                host.c_str(), device_port);

        if (startScreenCapture(device.preferred_adb_id, host, device_port)) {
            success_count++;
            MLOG_INFO("adb", "Success (port %d)", device_port);
        } else {
            MLOG_ERROR("adb", "Failed");
        }

        // Small delay between devices
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return success_count;
}

void AdbDeviceManager::assignPorts(int base_port) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate base port range
    if (base_port < 1024 || base_port > 65500) {
        MLOG_ERROR("adb", "Invalid base port %d, using default 5000", base_port);
        base_port = 5000;
    }

    int port_offset = 0;
    for (auto& [hw_id, device] : unique_devices_) {
        int assigned = base_port + port_offset;
        // Ensure port doesn't exceed valid range
        if (assigned > 65535) {
            MLOG_ERROR("adb", "Port overflow: cannot assign port %d to %s", assigned, device.display_name.c_str());
            device.assigned_port = 0;  // Invalid, skip this device
            continue;
        }
        device.assigned_port = assigned;
        MLOG_INFO("adb", "Assigned port %d to %s", device.assigned_port, device.display_name.c_str());
        port_offset++;
    }
}

int AdbDeviceManager::getAssignedPort(const std::string& hardware_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = unique_devices_.find(hardware_id);
    if (it != unique_devices_.end()) {
        return it->second.assigned_port;
    }
    return 0;
}

void AdbDeviceManager::setDevicePort(const std::string& hardware_id, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = unique_devices_.find(hardware_id);
    if (it != unique_devices_.end()) {
        it->second.assigned_port = port;
        MLOG_INFO("adb", "Assigned port %d to %s", port, it->second.display_name.c_str());
    }
}

bool AdbDeviceManager::getDeviceByPort(int port, UniqueDevice& out) const {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& [hw_id, device] : unique_devices_) {
        if (device.assigned_port == port) {
            out = device;
            return true;
        }
    }
    return false;
}

void AdbDeviceManager::sendTap(const std::string& adb_id, int x, int y) {
    std::string cmd = "shell input tap " + std::to_string(x) + " " + std::to_string(y);
    adbCommand(adb_id, cmd);
}

void AdbDeviceManager::sendSwipe(const std::string& adb_id, int x1, int y1, int x2, int y2, int duration_ms) {
    std::string cmd = "shell input swipe " +
                      std::to_string(x1) + " " + std::to_string(y1) + " " +
                      std::to_string(x2) + " " + std::to_string(y2) + " " +
                      std::to_string(duration_ms);
    adbCommand(adb_id, cmd);
}

void AdbDeviceManager::sendKey(const std::string& adb_id, int keycode) {
    std::string cmd = "shell input keyevent " + std::to_string(keycode);
    adbCommand(adb_id, cmd);
}

} // namespace gui
