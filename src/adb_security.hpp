#pragma once
// =============================================================================
// adb_security.hpp
// 
// Security validation functions for ADB operations.
// Extracted from adb_device_manager.cpp for testability.
// 
// These functions are the security boundary for all ADB command execution.
// =============================================================================

#include <string>
#include <cstring>
#include <cctype>
#include <regex>

namespace gui {
namespace security {

// Dangerous shell metacharacters that could enable command injection
constexpr const char* SHELL_METACHARACTERS = "|;&$`\\\"'<>(){}[]!#*?~\n\r";

/**
 * Validate ADB device ID format.
 * Valid formats:
 *   - Serial number: alphanumeric, may include ':', '.', '-', '_'
 *   - IP:port: xxx.xxx.xxx.xxx:port
 * 
 * @param adb_id  The device ID to validate
 * @return true if valid, false if potentially malicious
 */
inline bool isValidAdbId(const std::string& adb_id) {
    if (adb_id.empty() || adb_id.length() > 64) {
        return false;
    }

    // Check for shell metacharacters
    for (char c : adb_id) {
        if (std::strchr(SHELL_METACHARACTERS, c) != nullptr) {
            return false;
        }
    }

    // Must be alphanumeric, colons, dots, or hyphens
    for (char c : adb_id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != ':' && c != '.' && c != '-' && c != '_') {
            return false;
        }
    }

    return true;
}

/**
 * Sanitize command string for shell execution.
 * Returns sanitized string or empty string if input is dangerous.
 * 
 * @param command  Shell command to sanitize
 * @return Sanitized command, or empty string if blocked
 */
inline std::string sanitizeCommand(const std::string& command) {
    if (command.empty()) {
        return "";
    }

    // Check for extremely dangerous patterns
    static const std::regex dangerous_patterns(
        R"(\$\(|\`|;\s*rm|;\s*dd|>\s*/|<\s*/|\|\s*sh|\|\s*bash)",
        std::regex::icase
    );

    if (std::regex_search(command, dangerous_patterns)) {
        return "";
    }

    return command;
}

/**
 * Escape a string for safe use in shell commands.
 * 
 * @param arg  Shell argument to escape
 * @return Escaped argument string
 */
inline std::string escapeShellArg(const std::string& arg) {
    std::string escaped;
    escaped.reserve(arg.length() * 2);

    for (char c : arg) {
        if (std::strchr(SHELL_METACHARACTERS, c) != nullptr) {
            escaped += '\\';
        }
        escaped += c;
    }

    return escaped;
}

/**
 * Validate a remote file path for safe deletion.
 * Only allows paths in /data/local/tmp/ and /sdcard/.
 * 
 * @param remote_path  Path on the Android device
 * @return true if path is safe to operate on
 */
inline bool isAllowedRemotePath(const std::string& remote_path) {
    if (remote_path.empty()) {
        return false;
    }

    // Only allow specific directories
    if (remote_path.find("/data/local/tmp/") != 0 &&
        remote_path.find("/sdcard/") != 0) {
        return false;
    }

    // Check for shell metacharacters
    for (char c : remote_path) {
        if (std::strchr(SHELL_METACHARACTERS, c) != nullptr) {
            return false;
        }
    }

    return true;
}

/**
 * Determine connection type from ADB ID string.
 * WiFi format: IP:PORT (e.g., "192.168.0.5:5555")
 * USB format: alphanumeric serial
 * 
 * @param adb_id  Device identifier
 * @return "wifi" if IP:port format, "usb" otherwise
 */
inline std::string classifyConnectionString(const std::string& adb_id) {
    if (adb_id.find(':') != std::string::npos) {
        size_t colon_pos = adb_id.find(':');
        std::string ip_part = adb_id.substr(0, colon_pos);
        if (std::count(ip_part.begin(), ip_part.end(), '.') == 3) {
            return "wifi";
        }
    }
    return "usb";
}

/**
 * Extract IP address from WiFi ADB ID.
 * 
 * @param adb_id  WiFi device ID (e.g., "192.168.0.5:5555")
 * @return IP address, or empty string if not WiFi format
 */
inline std::string extractIp(const std::string& adb_id) {
    size_t colon_pos = adb_id.find(':');
    if (colon_pos != std::string::npos) {
        return adb_id.substr(0, colon_pos);
    }
    return "";
}

} // namespace security
} // namespace gui
