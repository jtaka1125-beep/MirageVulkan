#pragma once
// =============================================================================
// MirageSystem - Configuration Management
// =============================================================================
// Centralizes hardcoded paths and settings for external configuration.
// =============================================================================

#include <string>
#include <vector>
#include <fstream>
#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

namespace mirage::config {

/**
 * Application configuration with sane defaults.
 * Values can be overridden via config file or environment variables.
 */
struct MirageConfig {
    // ADB executable path (empty = use 'adb' from PATH)
    std::string adb_path;

    // Logging
    std::string log_directory;      // Default: exe directory or temp
    std::string log_filename = "mirage_vulkan.log";
    bool log_to_console = true;
    bool log_to_file = true;

    // Fonts (for ImGui Japanese rendering)
    std::vector<std::string> font_paths;

    // Temporary directory for screenshots, etc.
    std::string temp_directory;

    // ADB/USB tools
    std::string aoa_switch_path;    // Path to aoa_switch.exe
    std::string driver_installer_path;  // Path to install_android_winusb.py

    // Video settings
    int default_video_fps = 30;
    int max_video_width = 1920;
    int max_video_height = 1080;

    // Network
    int udp_listen_port = 5000;
    int tcp_video_base_port = 50100;

    // Default constructor with platform-specific defaults
    MirageConfig() {
        initDefaults();
    }

private:
    void initDefaults();
};

/**
 * Get the global system configuration instance.
 * Thread-safe, initialized on first access.
 */
inline MirageConfig& getSystemConfig() {
    static MirageConfig config;
    return config;
}

/**
 * Get executable directory (for relative path resolution).
 */
inline std::string getExeDirectory() {
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string exe_path(path);
    size_t pos = exe_path.find_last_of("\\/");
    return (pos != std::string::npos) ? exe_path.substr(0, pos) : ".";
#else
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) {
        path[len] = '\0';
        std::string exe_path(path);
        size_t pos = exe_path.find_last_of('/');
        return (pos != std::string::npos) ? exe_path.substr(0, pos) : ".";
    }
    return ".";
#endif
}

/**
 * Get user's home/appdata directory.
 */
inline std::string getUserDataDirectory() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path) == S_OK) {
        return std::string(path) + "\\MirageSystem";
    }
    return getExeDirectory();
#else
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    return std::string(home) + "/.mirage";
#endif
}

/**
 * Get system temp directory.
 */
inline std::string getTempDirectory() {
#ifdef _WIN32
    char temp[MAX_PATH];
    if (GetTempPathA(MAX_PATH, temp) > 0) {
        return std::string(temp);
    }
    return "C:\\Temp\\";
#else
    const char* tmp = getenv("TMPDIR");
    if (tmp) return std::string(tmp);
    return "/tmp/";
#endif
}

// Implementation of MirageConfig::initDefaults
inline void MirageConfig::initDefaults() {
    // Temp directory
    temp_directory = getTempDirectory();

    // Log directory - prefer user data, fallback to exe directory
    log_directory = getUserDataDirectory();

    // Font paths (Windows)
#ifdef _WIN32
    font_paths = {
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\YuGothR.ttc",
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\NotoSansJP-Regular.ttf",
        "C:\\Windows\\Fonts\\NotoSansCJK-Regular.ttc"
    };
#else
    font_paths = {
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    };
#endif

    // AOA switch tool - relative to exe
    std::string exe_dir = getExeDirectory();
#ifdef _WIN32
    aoa_switch_path = exe_dir + "\\aoa_switch.exe";
    driver_installer_path = exe_dir + "\\tools\\install_android_winusb.py";
#else
    aoa_switch_path = exe_dir + "/aoa_switch";
    driver_installer_path = exe_dir + "/tools/install_android_winusb.py";
#endif
}

/**
 * Load configuration from file (JSON-like simple format).
 * Returns true if file was loaded successfully.
 */
inline bool loadConfigFile(const std::string& path, MirageConfig& config) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line;
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        // Parse key=value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim whitespace
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);

        // Apply settings
        if (key == "log_directory") config.log_directory = value;
        else if (key == "log_filename") config.log_filename = value;
        else if (key == "adb_path") config.adb_path = value;
        else if (key == "log_to_console") config.log_to_console = (value == "true" || value == "1");
        else if (key == "log_to_file") config.log_to_file = (value == "true" || value == "1");
        else if (key == "temp_directory") config.temp_directory = value;
        else if (key == "aoa_switch_path") config.aoa_switch_path = value;
        else if (key == "driver_installer_path") config.driver_installer_path = value;
        else if (key == "default_video_fps") config.default_video_fps = std::stoi(value);
        else if (key == "max_video_width") config.max_video_width = std::stoi(value);
        else if (key == "max_video_height") config.max_video_height = std::stoi(value);
        else if (key == "udp_listen_port") config.udp_listen_port = std::stoi(value);
        else if (key == "tcp_video_base_port") config.tcp_video_base_port = std::stoi(value);
    }

    return true;
}

/**
 * Override config from environment variables.
 * Environment variables take precedence over config file.
 */
inline void applyEnvironmentOverrides(MirageConfig& config) {
    const char* val;

    if ((val = std::getenv("MIRAGE_LOG_DIR"))) config.log_directory = val;
    if ((val = std::getenv("MIRAGE_ADB_PATH"))) config.adb_path = val;
    if ((val = std::getenv("MIRAGE_TEMP_DIR"))) config.temp_directory = val;
    if ((val = std::getenv("MIRAGE_AOA_SWITCH"))) config.aoa_switch_path = val;
    if ((val = std::getenv("MIRAGE_VIDEO_FPS"))) config.default_video_fps = std::stoi(val);
    if ((val = std::getenv("MIRAGE_UDP_PORT"))) config.udp_listen_port = std::stoi(val);
}

} // namespace mirage::config
