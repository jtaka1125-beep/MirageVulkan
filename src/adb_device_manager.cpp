#include "adb_device_manager.hpp"
#include "auto_setup.hpp"
#include "config_loader.hpp"
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

        // Non-blocking read with 8s timeout (prevents hanging on unresponsive ADB)
        const DWORD EXEC_TIMEOUT_MS = 8000;
        DWORD start_tick = GetTickCount();
        char buffer[4096];
        while (true) {
            DWORD available = 0;
            if (!PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &available, nullptr)) break;
            if (available > 0) {
                DWORD bytesRead = 0;
                DWORD toRead = (available < sizeof(buffer) - 1) ? available : sizeof(buffer) - 1;
                ReadFile(hReadPipe, buffer, toRead, &bytesRead, nullptr);
                if (bytesRead > 0) { buffer[bytesRead] = '\0'; result += buffer; }
            } else {
                DWORD status = STILL_ACTIVE;
                GetExitCodeProcess(pi.hProcess, &status);
                if (status != STILL_ACTIVE) break;
                if (GetTickCount() - start_tick > EXEC_TIMEOUT_MS) {
                    TerminateProcess(pi.hProcess, 1);
                    break;
                }
                Sleep(25);
            }
        }
        WaitForSingleObject(pi.hProcess, 1000);
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

// Extract clean USB serial from ADB ID
// "adb-A9250700956-ieJaCE._adb-tls-connect._tcp" → "A9250700956"
// "A9250700956" → "A9250700956" (already clean)
// "192.168.0.6:5555" → "" (WiFi, no USB serial)
static std::string extractUsbSerial(const std::string& adb_id) {
    if (adb_id.size() > 4 && adb_id.substr(0, 4) == "adb-") {
        auto second_dash = adb_id.find('-', 4);
        if (second_dash != std::string::npos) {
            return adb_id.substr(4, second_dash - 4);
        }
    }
    // Already clean serial (no colons or dots = not an IP address)
    if (adb_id.find(':') == std::string::npos && adb_id.find('.') == std::string::npos) {
        return adb_id;
    }
    return "";  // WiFi connection, no USB serial
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

        // Ignore mDNS ADB records (e.g. adb-XXXX._adb-tls-connect._tcp)
        if (id.rfind("adb-", 0) == 0 && id.find("._adb") != std::string::npos) {
            continue;
        }

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
    // Phase 1: ポート割り当てを保存（要ロック）
    std::unordered_map<std::string, int> saved_tcp_ports;
    std::unordered_map<std::string, int> saved_assigned_ports;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [hw_id, ud] : unique_devices_) {
            if (ud.assigned_tcp_port > 0) {
                saved_tcp_ports[hw_id] = ud.assigned_tcp_port;
            }
            if (ud.assigned_port > 0) {
                saved_assigned_ports[hw_id] = ud.assigned_port;
            }
        }
    }

    // Phase 2: I/O実行（ロック不要 — adbCommand/getDevicePropはmutex_不使用）
    auto adb_ids = parseAdbDevices();

    std::map<std::string, DeviceInfo> new_devices;
    for (const auto& adb_id : adb_ids) {
        DeviceInfo info;
        info.adb_id = adb_id;
        info.conn_type = determineConnectionType(adb_id);
        info.is_online = true;

        // Get hardware ID
        info.hardware_id = getHardwareId(adb_id);

        // Get device model and serial (ro.serialno) for cross-matching
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

        // --- Android version / SDK ---
        info.android_version = getDeviceProp(adb_id, "ro.build.version.release");
        {
            std::string sdk_str = getDeviceProp(adb_id, "ro.build.version.sdk");
            if (!sdk_str.empty()) {
                try { info.sdk_level = std::stoi(sdk_str); } catch (...) {}
            }
        }

        // --- Screen resolution & density ---
        {
            std::string wm = adbCommand(adb_id, "shell wm size");
            parseScreenSize(wm, info.screen_width, info.screen_height);
            std::string density_out = adbCommand(adb_id, "shell wm density");
            // "Physical density: 480" or "Override density: 420"
            size_t colon = density_out.rfind(':');
            if (colon != std::string::npos) {
                std::string num = density_out.substr(colon + 1);
                try { info.screen_density = std::stoi(num); } catch (...) {}
            }
        }

        // --- Battery level ---
        {
            std::string bat = adbCommand(adb_id, "shell dumpsys battery");
            info.battery_level = parseBatteryLevel(bat);
        }

        new_devices[adb_id] = info;
    }

    // Phase 3: ロック取得してデータ更新
    std::lock_guard<std::mutex> lock(mutex_);

    devices_ = std::move(new_devices);
    unique_devices_.clear();

    // =========================================================================
    // hardware_id統一パス: android_id取得失敗のUSBデバイスをWiFiデバイスと照合
    // android_id成功時のフォーマット: "xxxxxxxx_NNNNNNN" (8文字+_+ハッシュ)
    // ro.serialnoフォールバック: "_"を含まない文字列
    // =========================================================================
    {
        // WiFiデバイスのIP→hardware_idマップを構築
        std::map<std::string, std::string> wifi_ip_to_hwid;
        // WiFiデバイスのmodel→hardware_idマップを構築 (IPマッチ失敗時のフォールバック)
        std::map<std::string, std::string> wifi_model_to_hwid;
        for (const auto& [adb_id, info] : devices_) {
            if (info.conn_type != ConnectionType::WiFi) continue;
            // android_idが正常取得できたWiFiデバイスのみ対象 ("_"を含む)
            if (info.hardware_id.find('_') == std::string::npos) continue;

            if (!info.ip_address.empty()) {
                wifi_ip_to_hwid[info.ip_address] = info.hardware_id;
            }
            if (!info.model.empty()) {
                wifi_model_to_hwid[info.model] = info.hardware_id;
            }
        }

        // USBデバイスでandroid_id取得失敗したものを照合
        for (auto& [adb_id, info] : devices_) {
            if (info.conn_type != ConnectionType::USB) continue;
            // android_idが正常取得できていれば統一不要 ("_"を含む)
            if (info.hardware_id.find('_') != std::string::npos) continue;

            std::string old_hwid = info.hardware_id;

            // 優先度1: IPアドレスでマッチ
            if (!info.ip_address.empty()) {
                auto it = wifi_ip_to_hwid.find(info.ip_address);
                if (it != wifi_ip_to_hwid.end()) {
                    info.hardware_id = it->second;
                    MLOG_INFO("adb", "デバイス統一(IP一致): USB:%s の hardware_id を %s → %s に更新 (IP=%s)",
                              adb_id.c_str(), old_hwid.c_str(), info.hardware_id.c_str(), info.ip_address.c_str());
                    continue;
                }
            }

            // 優先度2: モデル名でマッチ (同一モデルが1台のみの場合)
            if (!info.model.empty()) {
                // 同一モデルのWiFiデバイスが1つだけか確認
                int wifi_count = 0;
                std::string matched_hwid;
                for (const auto& [other_id, other_info] : devices_) {
                    if (other_info.conn_type != ConnectionType::WiFi) continue;
                    if (other_info.hardware_id.find('_') == std::string::npos) continue;
                    if (other_info.model == info.model) {
                        wifi_count++;
                        matched_hwid = other_info.hardware_id;
                    }
                }
                // 同一モデルが複数あると誤マッチするので1台のみの場合に限定
                if (wifi_count == 1) {
                    // さらに: このhardware_idのUSBデバイスがまだ統一されていないことを確認
                    bool already_unified = false;
                    for (const auto& [other_id, other_info] : devices_) {
                        if (other_id == adb_id) continue;
                        if (other_info.conn_type == ConnectionType::USB && other_info.hardware_id == matched_hwid) {
                            already_unified = true;
                            break;
                        }
                    }
                    if (!already_unified) {
                        info.hardware_id = matched_hwid;
                        MLOG_INFO("adb", "デバイス統一(モデル一致): USB:%s の hardware_id を %s → %s に更新 (model=%s)",
                                  adb_id.c_str(), old_hwid.c_str(), info.hardware_id.c_str(), info.model.c_str());
                        continue;
                    }
                }
            }
        }
    }

    // Build unique devices (hardware_id統一済みのdevices_からグルーピング)
    for (const auto& [adb_id, info] : devices_) {
        auto& unique = unique_devices_[info.hardware_id];
        unique.hardware_id = info.hardware_id;
        unique.model = info.model;
        unique.display_name = info.manufacturer + " " + info.model;
        unique.screen_width = info.screen_width;
        unique.screen_height = info.screen_height;
        unique.screen_density = info.screen_density;
        unique.android_version = info.android_version;
        unique.sdk_level = info.sdk_level;
        if (info.battery_level >= 0 || unique.battery_level < 0)
            unique.battery_level = info.battery_level;
        if (info.conn_type == ConnectionType::USB && unique.usb_serial.empty())
            unique.usb_serial = info.adb_id;

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

    // Restore saved port assignments
    for (auto& [hw_id, ud] : unique_devices_) {
        auto tcp_it = saved_tcp_ports.find(hw_id);
        if (tcp_it != saved_tcp_ports.end()) {
            ud.assigned_tcp_port = tcp_it->second;
        }
        auto port_it = saved_assigned_ports.find(hw_id);
        if (port_it != saved_assigned_ports.end()) {
            ud.assigned_port = port_it->second;
        }
    }

    

    // Apply fixed per-device tcp ports from devices.json (generated from device_profiles)
    try {
        auto& reg = mirage::config::ExpectedSizeRegistry::instance();
        reg.loadDevices("devices.json");
        for (const auto& kv : reg.allDevices()) {
            MLOG_INFO("adb", "Registry tcp_port entry: %s -> %d", kv.first.c_str(), kv.second.tcp_port);
        }
        for (auto& [hw_id, ud] : unique_devices_) {
            int p = 0;
            if (reg.getTcpPort(hw_id, p)) {
                ud.assigned_tcp_port = p;
                MLOG_INFO("adb", "Applied fixed tcp_port=%d to %s", p, hw_id.c_str());
            }
        }
    } catch (...) {
        // ignore
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

bool AdbDeviceManager::startScreenCapture(const std::string& adb_id, const std::string& host, int port, bool is_main) {
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
    auto result1 = setup.start_screen_capture(host, port, is_main);
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

    bool ok = (result2.status == mirage::SetupStatus::COMPLETED ||
              result2.status == mirage::SetupStatus::SKIPPED) &&
             (result3.status == mirage::SetupStatus::COMPLETED);

    if (ok) {
        int tcp_port = setup.get_tcp_port();
        MLOG_INFO("adb", "Success (port %d) - TCP mode on port %d", port, tcp_port);
        // Store TCP port for multi_device_receiver to use
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool found = false;
            for (auto& [key, ud] : unique_devices_) {
                MLOG_INFO("adb", "  tcp_port match check: key=%s preferred=%s vs adb_id=%s",
                          key.c_str(), ud.preferred_adb_id.c_str(), adb_id.c_str());
                if (ud.preferred_adb_id == adb_id) {
                    ud.assigned_tcp_port = tcp_port;
                    found = true;
                    MLOG_INFO("adb", "  -> matched by preferred_adb_id, tcp_port=%d", tcp_port);
                    break;
                }
            }
            if (!found) {
                // Fallback: check if adb_id appears in any wifi_connections or usb_connections
                for (auto& [key, ud] : unique_devices_) {
                    for (const auto& wc : ud.wifi_connections) {
                        if (wc == adb_id) {
                            ud.assigned_tcp_port = tcp_port;
                            found = true;
                            MLOG_INFO("adb", "  -> matched by wifi_connection, tcp_port=%d", tcp_port);
                            break;
                        }
                    }
                    if (found) break;
                    for (const auto& uc : ud.usb_connections) {
                        if (uc == adb_id) {
                            ud.assigned_tcp_port = tcp_port;
                            found = true;
                            MLOG_INFO("adb", "  -> matched by usb_connection, tcp_port=%d", tcp_port);
                            break;
                        }
                    }
                    if (found) break;
                }
            }
            if (!found) {
                MLOG_WARN("adb", "  -> NO MATCH for adb_id=%s, tcp_port lost!", adb_id.c_str());
            }
        }
    }
    return ok;
}

int AdbDeviceManager::startScreenCaptureOnAll(const std::string& host, int base_port) {
    // Assign ports only if base_port > 0 (otherwise pre-assigned via setDevicePort)
    if (base_port > 0) {
        assignPorts(base_port);
    }

    auto devices = getUniqueDevices();
    int success_count = 0;

    bool is_first_device = true;
    for (const auto& device : devices) {
        int device_port = device.assigned_port;
        MLOG_INFO("adb", "Starting screen capture on %s (%s) -> %s:%d (is_main=%d)", device.display_name.c_str(), device.preferred_adb_id.c_str(),
                host.c_str(), device_port, is_first_device);

        if (startScreenCapture(device.preferred_adb_id, host, device_port, is_first_device)) {
            success_count++;
            MLOG_INFO("adb", "Success (port %d)", device_port);
        } else {
            MLOG_ERROR("adb", "Failed");
        }
        is_first_device = false;

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

std::string AdbDeviceManager::resolveUsbSerial(const std::string& usb_serial) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Try cached usb_serial match
    for (const auto& [hw_id, dev] : unique_devices_) {
        if (!dev.usb_serial.empty() && dev.usb_serial == usb_serial) {
            return hw_id;
        }
        // Also check if usb_serial is contained in any usb_connections
        for (const auto& conn : dev.usb_connections) {
            if (conn.find(usb_serial) != std::string::npos) {
                return hw_id;
            }
        }
    }

    // 2. Fallback: query ro.serialno for devices with empty usb_serial
    for (auto& [hw_id, dev] : unique_devices_) {
        if (!dev.usb_serial.empty()) continue;

        // Find an ADB ID we can query
        std::string adb_id = dev.preferred_adb_id;
        if (adb_id.empty() && !dev.wifi_connections.empty()) {
            adb_id = dev.wifi_connections[0];
        }
        if (adb_id.empty() && !dev.usb_connections.empty()) {
            adb_id = dev.usb_connections[0];
        }
        if (adb_id.empty()) continue;

        // Copy adb_id locally, unlock mutex for the ADB call, then re-lock
        std::string adb_id_copy = adb_id;
        std::string hw_id_copy = hw_id;
        mutex_.unlock();
        std::string serialno = getDeviceProp(adb_id_copy, "ro.serialno");
        mutex_.lock();

        // Validate the result
        if (!serialno.empty() &&
            serialno.find("error") == std::string::npos &&
            serialno.find("unknown") == std::string::npos &&
            serialno.find("offline") == std::string::npos) {
            // Re-find the device after re-lock (map may have changed)
            auto it = unique_devices_.find(hw_id_copy);
            if (it != unique_devices_.end()) {
                it->second.usb_serial = serialno;
                MLOG_INFO("adb", "Resolved usb_serial for %s: %s (via ro.serialno)",
                          hw_id_copy.c_str(), serialno.c_str());

                if (serialno == usb_serial) {
                    return hw_id_copy;
                }
            }
        }
    }

    return "";
}

void AdbDeviceManager::queryScreenInfo(const std::string& adb_id) {
    // デバイス存在確認 & hardware_id取得 (ロック内)
    std::string hardware_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(adb_id);
        if (it == devices_.end()) return;
        hardware_id = it->second.hardware_id;
    }

    // ブロッキングI/Oはロック外で実行
    std::string wm = adbCommand(adb_id, "shell wm size");
    std::string bat = adbCommand(adb_id, "shell dumpsys battery");

    int screen_w = 0, screen_h = 0;
    parseScreenSize(wm, screen_w, screen_h);
    int battery = parseBatteryLevel(bat);

    // 結果をロック内で書き込み
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(adb_id);
    if (it == devices_.end()) return;  // refresh()で消えた可能性
    auto& info = it->second;
    info.screen_width = screen_w;
    info.screen_height = screen_h;
    info.battery_level = battery;

    // unique deviceも更新
    auto uit = unique_devices_.find(hardware_id);
    if (uit != unique_devices_.end()) {
        uit->second.screen_width = screen_w;
        uit->second.screen_height = screen_h;
        uit->second.battery_level = battery;
    }
}

// static
int AdbDeviceManager::parseBatteryLevel(const std::string& s) {
    // "dumpsys battery" output contains line: "  level: 78"
    size_t pos = s.find("level:");
    if (pos == std::string::npos) return -1;
    pos += 6; // skip "level:"
    // skip whitespace
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
    if (pos >= s.size()) return -1;
    try {
        size_t end;
        int v = std::stoi(s.substr(pos), &end);
        if (v >= 0 && v <= 100) return v;
    } catch (...) {}
    return -1;
}

// static
bool AdbDeviceManager::parseScreenSize(const std::string& s, int& w, int& h) {
    // "wm size" output: "Physical size: 1080x2400" or "Override size: 1080x2400"
    size_t pos = s.find('x');
    if (pos == std::string::npos || pos == 0) return false;
    // find start of width number (go back from 'x')
    size_t w_end = pos;
    size_t w_start = w_end;
    while (w_start > 0 && std::isdigit((unsigned char)s[w_start - 1])) w_start--;
    if (w_start == w_end) return false;
    try {
        w = std::stoi(s.substr(w_start, w_end - w_start));
        h = std::stoi(s.substr(pos + 1));
        return (w > 0 && h > 0);
    } catch (...) {}
    return false;
}

} // namespace gui
