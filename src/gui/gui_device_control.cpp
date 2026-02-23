// =============================================================================
// MirageSystem v2 GUI - Device Control Implementation
// =============================================================================
// AOA mode switching and ADB connection management
// =============================================================================
#include "mirage_log.hpp"
#include "gui_device_control.hpp"
#include "gui_state.hpp"
#include "winusb_checker.hpp"

#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <future>
#include <thread>
#include <Windows.h>

namespace mirage::gui::device_control {

using namespace mirage::gui::state;

// Track async operations to avoid detached threads on shutdown
static std::vector<std::future<void>> s_async_ops;
static std::mutex s_async_mutex;

static void trackAsync(std::future<void> fut) {
    std::lock_guard<std::mutex> lock(s_async_mutex);
    // Clean up completed futures
    s_async_ops.erase(
        std::remove_if(s_async_ops.begin(), s_async_ops.end(),
            [](const std::future<void>& f) {
                return f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
            }),
        s_async_ops.end());
    s_async_ops.push_back(std::move(fut));
}

// =============================================================================
// Internal Helpers
// =============================================================================

// Get directory of current executable
static std::string getExeDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string exePath(path);
    size_t pos = exePath.find_last_of("\\/");
    return (pos != std::string::npos) ? exePath.substr(0, pos) : ".";
}

// Validate device ID to prevent command injection
// Device IDs should only contain alphanumeric chars, hyphens, underscores, colons, dots
static bool isValidDeviceId(const std::string& device_id) {
    if (device_id.empty() || device_id.size() > 256) return false;
    for (char c : device_id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '-' && c != '_' && c != ':' && c != '.') {
            return false;
        }
    }
    return true;
}

// Non-blocking process execution using CreateProcess
static bool executeProcess(const std::string& cmd_line, int timeout_ms = 30000) {
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmd_copy = cmd_line;

    MLOG_INFO("devctl", "Executing: %s", cmd_line.c_str());

    if (!CreateProcessA(nullptr, cmd_copy.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        MLOG_ERROR("devctl", "CreateProcess failed: %lu", GetLastError());
        return false;
    }

    DWORD wait_result = WaitForSingleObject(pi.hProcess, timeout_ms);
    bool success = false;

    if (wait_result == WAIT_TIMEOUT) {
        MLOG_INFO("devctl", "Process timed out after %dms", timeout_ms);
        TerminateProcess(pi.hProcess, 1);
    } else {
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        success = (exit_code == 0);
        MLOG_INFO("devctl", "Process exited with code: %lu", exit_code);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return success;
}

static bool executeAOASwitch(const std::string& device_id = "") {
    if (!device_id.empty() && !isValidDeviceId(device_id)) {
        MLOG_ERROR("devctl", "Invalid device ID rejected: %s", device_id.c_str());
        return false;
    }

    std::string exeDir = getExeDir();
    std::string cmd = "\"" + exeDir + "\\aoa_switch.exe\"";
    if (!device_id.empty()) {
        cmd += " --device " + device_id;
    } else {
        cmd += " --all";
    }

    return executeProcess(cmd);
}

// Validate IP address format (simple check: digits and dots only)
static bool isValidIpAddress(const std::string& ip) {
    if (ip.empty() || ip.size() > 45) return false;  // Max IPv6 length
    for (char c : ip) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != ':') {
            return false;
        }
    }
    return true;
}

static bool executeADB(const std::string& args) {
    std::string cmd = "adb " + args;
    return executeProcess(cmd, 15000);  // 15s timeout for ADB
}

// =============================================================================
// AOA Mode Control
// =============================================================================

int switchAllDevicesToAOA() {
    MLOG_INFO("devctl", "Switching all devices to AOA mode...");

    auto gui = g_gui;  // thread-safe local copy
    if (gui) {
        gui->logInfo(u8"AOAモード切替中...");
    }

    // Run in background thread to avoid blocking GUI
    trackAsync(std::async(std::launch::async, []() {
        bool success = executeAOASwitch();
        auto gui = g_gui;

        if (success) {
            // Rescan USB devices after AOA switch
            if (g_hybrid_cmd) {
                g_hybrid_cmd->rescan();
            }
            int count = g_hybrid_cmd ? g_hybrid_cmd->device_count() : 0;
            if (gui) {
                gui->logInfo(u8"AOAモード切替完了: " + std::to_string(count) + u8"台");
            }
        } else {
            if (gui) {
                gui->logError(u8"AOAモード切替失敗");
            }
        }
    }));

    return 0;  // Async - result will be logged later
}

bool switchDeviceToAOA(const std::string& device_id) {
    MLOG_INFO("devctl", "Switching device %s to AOA mode...", device_id.c_str());

    auto gui = g_gui;
    if (gui) {
        gui->logInfo("AOA mode switch: " + device_id);
    }

    return executeAOASwitch(device_id);
}

bool isDeviceInAOAMode(const std::string& device_id) {
    if (!g_hybrid_cmd) return false;
    return g_hybrid_cmd->is_device_connected(device_id);
}

// =============================================================================
// ADB Connection Control
// =============================================================================

bool connectDeviceADB(const std::string& device_id) {
    MLOG_INFO("devctl", "Connecting device %s via ADB...", device_id.c_str());

    if (!g_adb_manager) {
        MLOG_INFO("devctl", "ADB manager not available");
        return false;
    }

    // Get device info
    ::gui::AdbDeviceManager::UniqueDevice dev_info;
    if (!g_adb_manager->getUniqueDevice(device_id, dev_info)) {
        MLOG_INFO("devctl", "Device not found: %s", device_id.c_str());
        return false;
    }

    // Try USB first
    if (!dev_info.usb_connections.empty()) {
        auto gui = g_gui;
        if (gui) {
            gui->logInfo("ADB connected (USB): " + device_id);
        }
        return true;  // Already connected via USB
    }

    // Try WiFi ADB (async to avoid blocking GUI)
    if (!dev_info.ip_address.empty() && isValidIpAddress(dev_info.ip_address)) {
        std::string ip = dev_info.ip_address;
        std::string dev_id_copy = device_id;
        trackAsync(std::async(std::launch::async, [ip, dev_id_copy]() {
            std::string connect_cmd = "connect " + ip + ":5555";
            bool success = executeADB(connect_cmd);
            auto gui = g_gui;
            if (gui) {
                if (success) {
                    gui->logInfo("ADB connected (WiFi): " + ip);
                } else {
                    gui->logWarning("ADB connection failed: " + dev_id_copy);
                }
            }
        }));
        return true;  // Async - result logged later
    }

    auto gui = g_gui;
    if (gui) {
        gui->logWarning("ADB connection failed: " + device_id);
    }
    return false;
}

bool disconnectDeviceADB(const std::string& device_id) {
    if (!g_adb_manager) return false;

    ::gui::AdbDeviceManager::UniqueDevice dev_info;
    if (!g_adb_manager->getUniqueDevice(device_id, dev_info)) {
        return false;
    }

    if (!dev_info.ip_address.empty() && isValidIpAddress(dev_info.ip_address)) {
        std::string disconnect_cmd = "disconnect " + dev_info.ip_address + ":5555";
        return executeADB(disconnect_cmd);
    }

    return false;
}

bool hasADBConnection(const std::string& device_id) {
    if (!g_adb_manager) return false;

    ::gui::AdbDeviceManager::UniqueDevice dev_info;
    if (!g_adb_manager->getUniqueDevice(device_id, dev_info)) {
        return false;
    }

    return !dev_info.usb_connections.empty() || !dev_info.wifi_connections.empty();
}

std::string getADBConnectionType(const std::string& device_id) {
    if (!g_adb_manager) return "none";

    ::gui::AdbDeviceManager::UniqueDevice dev_info;
    if (!g_adb_manager->getUniqueDevice(device_id, dev_info)) {
        return "none";
    }

    if (!dev_info.usb_connections.empty()) return "usb";
    if (!dev_info.wifi_connections.empty()) return "wifi";
    return "none";
}

// =============================================================================
// Device Info
// =============================================================================

std::vector<DeviceControlInfo> getAllDeviceControlInfo() {
    std::vector<DeviceControlInfo> result;

    if (!g_adb_manager) return result;

    auto devices = g_adb_manager->getUniqueDevices();
    for (const auto& dev : devices) {
        DeviceControlInfo info;
        info.device_id = dev.hardware_id;
        info.display_name = dev.display_name;
        info.in_aoa_mode = isDeviceInAOAMode(dev.hardware_id);
        info.has_adb = !dev.usb_connections.empty() || !dev.wifi_connections.empty();
        info.adb_type = getADBConnectionType(dev.hardware_id);
        info.ip_address = dev.ip_address;
        info.screen_width = dev.screen_width;
        info.screen_height = dev.screen_height;
        info.android_version = dev.android_version;
        info.sdk_level = dev.sdk_level;
        info.battery_level = dev.battery_level;
        result.push_back(info);
    }

    return result;
}

DeviceControlInfo getDeviceControlInfo(const std::string& device_id) {
    DeviceControlInfo info;
    info.device_id = device_id;
    info.display_name = device_id;
    info.in_aoa_mode = false;
    info.has_adb = false;
    info.adb_type = "none";

    if (!g_adb_manager) return info;

    ::gui::AdbDeviceManager::UniqueDevice dev_info;
    if (g_adb_manager->getUniqueDevice(device_id, dev_info)) {
        info.display_name = dev_info.display_name;
        info.in_aoa_mode = isDeviceInAOAMode(device_id);
        info.has_adb = !dev_info.usb_connections.empty() || !dev_info.wifi_connections.empty();
        info.adb_type = getADBConnectionType(device_id);
        info.ip_address = dev_info.ip_address;
        info.screen_width = dev_info.screen_width;
        info.screen_height = dev_info.screen_height;
        info.android_version = dev_info.android_version;
        info.sdk_level = dev_info.sdk_level;
        info.battery_level = dev_info.battery_level;
    }

    return info;
}

// =============================================================================
// GUI Rendering
// =============================================================================

bool renderSwitchAllAOAButton() {
    // Large button with icon
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.4f, 0.7f, 1.0f));

    bool clicked = ImGui::Button("All Devices AOA Mode", ImVec2(200, 40));

    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Switch all devices to AOA mode\nPress once at startup");
    }

    if (clicked) {
        switchAllDevicesToAOA();
    }

    return clicked;
}

bool renderDeviceADBButton(const std::string& device_id) {
    auto info = getDeviceControlInfo(device_id);

    // Button color based on connection state
    if (info.has_adb) {
        if (info.adb_type == "usb") {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));  // Green
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.7f, 0.2f, 1.0f));  // Yellow (WiFi)
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));  // Gray
    }
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.6f, 0.8f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));

    // Create unique button ID
    std::string button_label = info.has_adb ?
        (info.adb_type == "usb" ? "ADB(USB)" : "ADB(WiFi)") :
        "ADB Connect";
    button_label += "##" + device_id;

    bool clicked = ImGui::Button(button_label.c_str(), ImVec2(80, 25));

    ImGui::PopStyleColor(3);

    // Tooltip
    if (ImGui::IsItemHovered()) {
        std::string tooltip = info.display_name + "\n";
        tooltip += "AOA: " + std::string(info.in_aoa_mode ? "Connected" : "Disconnected") + "\n";
        tooltip += "ADB: " + info.adb_type;
        if (!info.ip_address.empty()) {
            tooltip += "\nIP: " + info.ip_address;
        }
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    if (clicked) {
        if (!info.has_adb) {
            connectDeviceADB(device_id);
        }
        // If already connected, clicking could toggle or show menu
        // For now, just log
        auto gui = g_gui;
        if (gui) {
            gui->logInfo("ADB button clicked: " + device_id);
        }
    }

    return clicked;
}

void renderDeviceControlPanel() {
    // Position in top-right area, below title bar - always set position on first frame
    static bool first_frame = true;
    ImGuiIO& io = ImGui::GetIO();
    float panel_width = 280.0f;

    if (first_frame) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panel_width - 10, 30));
        ImGui::SetNextWindowCollapsed(true);  // Start collapsed
        first_frame = false;
    }

    if (!ImGui::Begin("Device Control", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    // Header with "Switch All to AOA" button
    renderSwitchAllAOAButton();

    ImGui::Separator();
    ImGui::Text("Connected Devices:");
    ImGui::Separator();

    // Device list
    auto devices = getAllDeviceControlInfo();

    if (devices.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No devices found");
    } else {
        for (const auto& dev : devices) {
            ImGui::PushID(dev.device_id.c_str());

            // Device name
            ImGui::Text("%s", dev.display_name.c_str());
            // Detail row: battery, resolution, Android version
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            if (dev.battery_level >= 0) {
                ImVec4 bat_col = dev.battery_level > 20
                    ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                    : ImVec4(0.95f, 0.3f, 0.3f, 1.0f);
                ImGui::SameLine();
                ImGui::TextColored(bat_col, "[%d%%]", dev.battery_level);
            }
            if (dev.screen_width > 0) {
                ImGui::SameLine();
                ImGui::Text("%dx%d", dev.screen_width, dev.screen_height);
            }
            if (!dev.android_version.empty()) {
                ImGui::SameLine();
                ImGui::Text("A%s", dev.android_version.c_str());
            }
            ImGui::PopStyleColor();
            ImGui::SameLine(200);

            // AOA status
            if (dev.in_aoa_mode) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[AOA]");
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[---]");
            }
            ImGui::SameLine(260);

            // ADB button
            renderDeviceADBButton(dev.device_id);

            // Individual AOA button (optional)
            ImGui::SameLine();
            if (!dev.in_aoa_mode) {
                if (ImGui::SmallButton(("AOA##" + dev.device_id).c_str())) {
                    switchDeviceToAOA(dev.device_id);
                }
            }

            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // Status summary
    int aoa_count = 0;
    int adb_count = 0;
    for (const auto& dev : devices) {
        if (dev.in_aoa_mode) aoa_count++;
        if (dev.has_adb) adb_count++;
    }
    ImGui::Text("AOA: %d / ADB: %d / Total: %d",
                aoa_count, adb_count, (int)devices.size());

    // WinUSB driver warning when no AOA devices found but ADB devices exist
    if (aoa_count == 0 && adb_count > 0) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
            "USB AOA: WinUSB driver required");
        ImGui::TextWrapped(
            "ADB fallback active. For lower-latency USB control,\n"
            "install WinUSB driver via [Driver Setup] button above.");

        // Cache WinUSB check (run at most once per 10 seconds)
        static bool s_winusb_checked = false;
        static bool s_winusb_needed = false;
        static auto s_last_check = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (!s_winusb_checked || (now - s_last_check) > std::chrono::seconds(10)) {
            s_winusb_needed = mirage::WinUsbChecker::anyDeviceNeedsWinUsb();
            s_winusb_checked = true;
            s_last_check = now;
        }

        if (s_winusb_needed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.3f, 0.1f, 1.0f));
            if (ImGui::Button("Install WinUSB Driver", ImVec2(200, 30))) {
                std::string exeDir = getExeDir();
                std::string script = exeDir + "\\install_android_winusb.py";
                mirage::WinUsbChecker::launchInstaller(script);
            }
            ImGui::PopStyleColor(3);
        }
    }

    ImGui::End();
}

} // namespace mirage::gui::device_control
