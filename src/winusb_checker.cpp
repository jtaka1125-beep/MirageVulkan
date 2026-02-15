#include "winusb_checker.hpp"
#include "mirage_log.hpp"

#include <windows.h>
#include <shellapi.h>
#include <sstream>
#include <array>
#include <memory>

namespace mirage {

namespace {
// Execute command without showing console window, returns output
std::string execCommandHidden(const std::string& cmd) {
    std::string result;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return result;
    }

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = nullptr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmd_copy = cmd;

    if (CreateProcessA(nullptr, cmd_copy.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWritePipe);
        hWritePipe = nullptr;

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
} // namespace

// Known Android vendor IDs
static const char* ANDROID_VIDS[] = {
    "18D1", // Google
    "04E8", // Samsung
    "22B8", // Motorola
    "2717", // Xiaomi
    "2A70", // OnePlus
    "0E8D", // MediaTek
    "1782", // Spreadtrum
    "1F3A", // Allwinner
    "2207", // Rockchip
    "0BB4", // HTC
    "1004", // LG
    "0FCE", // Sony
    "12D1", // Huawei
    "2C7C", // Quectel (some Android devices)
    nullptr
};

bool WinUsbChecker::isAndroidVid(const std::string& vid) {
    for (int i = 0; ANDROID_VIDS[i]; i++) {
        if (vid == ANDROID_VIDS[i]) return true;
    }
    return false;
}

// ============================================================================
// Testable parser: takes raw pipe-delimited output, returns parsed devices
// ============================================================================
std::vector<WinUsbChecker::UsbDeviceStatus> WinUsbChecker::parseDeviceOutput(const std::string& raw_output) {
    std::vector<UsbDeviceStatus> results;

    std::istringstream iss(raw_output);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim trailing whitespace/newlines
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (line.empty()) continue;

        // Split by '|' - expect: VID|PID|FriendlyName|InstanceId|Service
        std::vector<std::string> parts;
        std::istringstream ls(line);
        std::string part;
        while (std::getline(ls, part, '|')) {
            parts.push_back(part);
        }

        // Need at least 4 fields (service can be empty = missing 5th field)
        if (parts.size() < 4) continue;

        UsbDeviceStatus status;
        status.vid = parts[0];
        status.pid = parts[1];
        status.name = parts[2];
        status.instance_id = parts[3];
        status.current_driver = (parts.size() >= 5 && !parts[4].empty()) ? parts[4] : "None";

        // Only include Android devices
        if (!isAndroidVid(status.vid)) continue;

        // Check if WinUSB is needed (case-insensitive)
        std::string drv_lower = status.current_driver;
        for (auto& ch : drv_lower) ch = static_cast<char>(tolower(ch));
        status.needs_winusb = (drv_lower != "winusb");

        results.push_back(status);
    }

    return results;
}

// ============================================================================
// Build diagnostic summary from pre-parsed devices
// ============================================================================
std::string WinUsbChecker::buildDiagnosticSummary(const std::vector<UsbDeviceStatus>& devices) {
    if (devices.empty()) {
        return "No Android USB devices detected";
    }

    int ok_count = 0, needs_count = 0;
    std::string details;
    for (const auto& d : devices) {
        if (d.needs_winusb) {
            needs_count++;
            if (!details.empty()) details += ", ";
            details += d.name + " (VID=" + d.vid + " driver=" + d.current_driver + ")";
        } else {
            ok_count++;
        }
    }

    std::string summary = std::to_string(ok_count) + " device(s) OK";
    if (needs_count > 0) {
        summary += ", " + std::to_string(needs_count) + " need(s) WinUSB: " + details;
    }
    return summary;
}

// ============================================================================
// Live device check (PowerShell)
// ============================================================================
std::vector<WinUsbChecker::UsbDeviceStatus> WinUsbChecker::checkDevices() {
    const char* ps_script = R"PS(
$devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -like "USB\VID_*" -and $_.Status -eq "OK"
}
foreach ($dev in $devices) {
    $id = $dev.InstanceId
    if ($id -match "VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})") {
        $vid = $Matches[1].ToUpper()
        try {
            $svc = (Get-PnpDeviceProperty -InstanceId $id -KeyName "DEVPKEY_Device_Service" -ErrorAction SilentlyContinue).Data
        } catch { $svc = "" }
        if (-not $svc) { $svc = "" }
        Write-Output "$vid|$($Matches[2].ToUpper())|$($dev.FriendlyName)|$id|$svc"
    }
}
)PS";

    std::string cmd = "powershell -NoProfile -NoLogo -Command \"" + std::string(ps_script) + "\"";

    std::string output = execCommandHidden(cmd);
    if (output.empty()) {
        MLOG_ERROR("winusb", "Failed to execute PowerShell for device check");
        return {};
    }

    auto results = parseDeviceOutput(output);

    // Log each device
    for (const auto& s : results) {
        MLOG_INFO("winusb", "Device VID=%s PID=%s (%s) driver=%s %s",
                  s.vid.c_str(), s.pid.c_str(), s.name.c_str(),
                  s.current_driver.c_str(),
                  s.needs_winusb ? "-> NEEDS WinUSB" : "OK");
    }

    return results;
}

bool WinUsbChecker::anyDeviceNeedsWinUsb() {
    auto devices = checkDevices();
    for (const auto& d : devices) {
        if (d.needs_winusb) return true;
    }
    return false;
}

std::string WinUsbChecker::getDiagnosticSummary() {
    return buildDiagnosticSummary(checkDevices());
}

bool WinUsbChecker::launchInstaller(const std::string& script_path) {
    std::string params = "\"" + script_path + "\"";
    HINSTANCE result = ShellExecuteA(nullptr, "runas", "python",
                                      params.c_str(), nullptr, SW_SHOW);
    bool ok = reinterpret_cast<intptr_t>(result) > 32;
    if (ok) {
        MLOG_INFO("winusb", "WinUSB installer launched: %s", script_path.c_str());
    } else {
        MLOG_ERROR("winusb", "Failed to launch WinUSB installer (error=%lld)",
                   static_cast<long long>(reinterpret_cast<intptr_t>(result)));
    }
    return ok;
}

} // namespace mirage
