#pragma once
// =============================================================================
// MirageTestKit Config Loader
// =============================================================================
// Loads settings from config.json with nlohmann/json (fallback to manual parser)
// =============================================================================

#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include "mirage_log.hpp"

// Try nlohmann/json, fall back to manual parser
#if __has_include("nlohmann/json.hpp")
#include "nlohmann/json.hpp"
#define MIRAGE_HAS_JSON 1
#else
#define MIRAGE_HAS_JSON 0
#endif

namespace mirage {
namespace config {

struct NetworkConfig {
    std::string pc_ip = "192.168.0.7";
    int video_base_port = 60000;
    int command_base_port = 50000;
    int tcp_command_port = 50100;
};

struct UsbTetherConfig {
    std::string android_ip = "192.168.42.129";
    std::string pc_subnet = "192.168.42.0/24";
};

struct GuiConfig {
    int window_width = 1920;
    int window_height = 1080;
    bool vsync = true;
};

struct AiConfig {
    bool enabled = true;
    std::string templates_dir = "templates";
    float default_threshold = 0.80f;
    // VisionDecisionEngine settings (configurable via config.json)
    int  vde_confirm_count      = 3;
    int  vde_cooldown_ms        = 2000;
    int  vde_debounce_window_ms = 500;
    // Layer 3 (OllamaVision)
    bool vde_enable_layer3          = false;
    int  vde_layer3_no_match_frames = 150;
    int  vde_layer3_stuck_frames    = 300;
    int  vde_layer3_no_match_ms     = 5000;
    int  vde_layer3_cooldown_ms     = 30000;
};

struct OcrConfig {
    bool enabled = false;
    std::string language = "eng+jpn";
};

struct LogConfig {
    std::string log_path = "mirage_gui.log";
};

struct AppConfig {
    NetworkConfig network;
    UsbTetherConfig usb_tether;
    GuiConfig gui;
    AiConfig ai;
    OcrConfig ocr;
    LogConfig log;
};

#if MIRAGE_HAS_JSON
// Safe JSON accessor with section/key and default value
template<typename T>
T jsonGet(const nlohmann::json& j, const std::string& section,
          const std::string& key, const T& def) {
    try {
        if (j.contains(section) && j[section].contains(key)) {
            return j[section][key].get<T>();
        }
    } catch (...) {}
    return def;
}
#else
// Fallback: simple JSON extractors (no external deps)
inline std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('\"', pos);
    if (pos == std::string::npos) return "";
    size_t start = pos + 1;
    size_t end = json.find('\"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

inline int extractJsonInt(const std::string& json, const std::string& key, int def) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string numStr;
    while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '-')) numStr += json[pos++];
    if (numStr.empty()) return def;
    try { return std::stoi(numStr); } catch (...) { return def; }
}

inline float extractJsonFloat(const std::string& json, const std::string& key, float def) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string numStr;
    while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '-' || json[pos] == '.')) numStr += json[pos++];
    if (numStr.empty()) return def;
    try { return std::stof(numStr); } catch (...) { return def; }
}

inline bool extractJsonBool(const std::string& json, const std::string& key, bool def) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    if (json.find("true", pos) < json.find(',', pos)) return true;
    if (json.find("false", pos) < json.find(',', pos)) return false;
    return def;
}
#endif

// @param configPath  Path to config file
// @param strict      If true, only try the exact path (no fallback search)
inline AppConfig loadConfig(const std::string& configPath = "../config.json",
                            bool strict = false) {
    AppConfig config;

    std::ifstream file(configPath);
    if (!file.is_open() && !strict) {
        file.open("config.json");
        if (!file.is_open()) {
            file.open("../../config.json");
        }
    }
    if (!file.is_open()) {
        MLOG_WARN("config", "config.json not found, using defaults");
        return config;
    }

#if MIRAGE_HAS_JSON
    try {
        nlohmann::json j = nlohmann::json::parse(file);

        config.network.pc_ip = jsonGet<std::string>(j, "network", "pc_ip", "192.168.0.7");
        config.network.video_base_port = jsonGet<int>(j, "network", "video_base_port", 60000);
        config.network.command_base_port = jsonGet<int>(j, "network", "command_base_port", 50000);
        config.network.tcp_command_port = jsonGet<int>(j, "network", "tcp_command_port", 50100);

        config.usb_tether.android_ip = jsonGet<std::string>(j, "usb_tether", "android_ip", "192.168.42.129");
        config.usb_tether.pc_subnet = jsonGet<std::string>(j, "usb_tether", "pc_subnet", "192.168.42.0/24");

        config.gui.window_width = jsonGet<int>(j, "gui", "window_width", 1920);
        config.gui.window_height = jsonGet<int>(j, "gui", "window_height", 1080);
        config.gui.vsync = jsonGet<bool>(j, "gui", "vsync", true);

        config.ai.enabled = jsonGet<bool>(j, "ai", "enabled", true);
        config.ai.templates_dir = jsonGet<std::string>(j, "ai", "templates_dir", "templates");
        config.ai.default_threshold = jsonGet<float>(j, "ai", "default_threshold", 0.80f);
        config.ai.vde_confirm_count = jsonGet<int>(j, "ai", "vde_confirm_count", 3);
        config.ai.vde_cooldown_ms = jsonGet<int>(j, "ai", "vde_cooldown_ms", 2000);
        config.ai.vde_debounce_window_ms = jsonGet<int>(j, "ai", "vde_debounce_window_ms", 500);
        // Layer 3
        config.ai.vde_enable_layer3       = jsonGet<bool>(j, "ai", "vde_enable_layer3", false);
        config.ai.vde_layer3_no_match_frames = jsonGet<int>(j, "ai", "vde_layer3_no_match_frames", 150);
        config.ai.vde_layer3_stuck_frames = jsonGet<int>(j, "ai", "vde_layer3_stuck_frames", 300);
        config.ai.vde_layer3_no_match_ms  = jsonGet<int>(j, "ai", "vde_layer3_no_match_ms", 5000);
        config.ai.vde_layer3_cooldown_ms  = jsonGet<int>(j, "ai", "vde_layer3_cooldown_ms", 30000);

        config.ocr.enabled = jsonGet<bool>(j, "ocr", "enabled", false);
        config.ocr.language = jsonGet<std::string>(j, "ocr", "language", "eng+jpn");

        config.log.log_path = jsonGet<std::string>(j, "log", "log_path", "mirage_gui.log");

    } catch (const nlohmann::json::exception& e) {
        MLOG_ERROR("config", "JSON parse error: %s", e.what());
        return config;
    }
#else
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();

    config.network.pc_ip = extractJsonString(json, "pc_ip");
    if (config.network.pc_ip.empty()) config.network.pc_ip = "192.168.0.7";
    config.network.video_base_port = extractJsonInt(json, "video_base_port", 60000);
    config.network.command_base_port = extractJsonInt(json, "command_base_port", 50000);
    config.network.tcp_command_port = extractJsonInt(json, "tcp_command_port", 50100);

    config.usb_tether.android_ip = extractJsonString(json, "android_ip");
    if (config.usb_tether.android_ip.empty()) config.usb_tether.android_ip = "192.168.42.129";

    config.gui.window_width = extractJsonInt(json, "window_width", 1920);
    config.gui.window_height = extractJsonInt(json, "window_height", 1080);
    config.gui.vsync = extractJsonBool(json, "vsync", true);

    config.ai.enabled = extractJsonBool(json, "enabled", true);
    config.ai.templates_dir = extractJsonString(json, "templates_dir");
    if (config.ai.templates_dir.empty()) config.ai.templates_dir = "templates";
    config.ai.default_threshold = extractJsonFloat(json, "default_threshold", 0.80f);
    config.ai.vde_confirm_count = extractJsonInt(json, "vde_confirm_count", 3);
    config.ai.vde_cooldown_ms = extractJsonInt(json, "vde_cooldown_ms", 2000);
    config.ai.vde_debounce_window_ms = extractJsonInt(json, "vde_debounce_window_ms", 500);
    // Layer 3
    config.ai.vde_enable_layer3       = extractJsonBool(json, "vde_enable_layer3", false);
    config.ai.vde_layer3_no_match_frames = extractJsonInt(json, "vde_layer3_no_match_frames", 150);
    config.ai.vde_layer3_stuck_frames = extractJsonInt(json, "vde_layer3_stuck_frames", 300);
    config.ai.vde_layer3_no_match_ms  = extractJsonInt(json, "vde_layer3_no_match_ms", 5000);
    config.ai.vde_layer3_cooldown_ms  = extractJsonInt(json, "vde_layer3_cooldown_ms", 30000);

    config.ocr.enabled = extractJsonBool(json, "enabled", false);
    config.ocr.language = extractJsonString(json, "language");
    if (config.ocr.language.empty()) config.ocr.language = "eng+jpn";

    config.log.log_path = extractJsonString(json, "log_path");
    if (config.log.log_path.empty()) config.log.log_path = "mirage_gui.log";
#endif

    MLOG_INFO("config", "Loaded: pc_ip=%s, video_port=%d, command_port=%d",
              config.network.pc_ip.c_str(),
              config.network.video_base_port,
              config.network.command_base_port);

    return config;
}

inline AppConfig& getConfig() {
    static AppConfig config = loadConfig();
    return config;
}

// =============================================================================
// Device Registry - loads expected resolution from devices.json
// =============================================================================
struct ExpectedDeviceSpec {
    std::string hardware_id;
    int screen_width = 0;
    int screen_height = 0;
    int screen_density = 0;
    int tcp_port = 0;
};

class ExpectedSizeRegistry {
public:
    static ExpectedSizeRegistry& instance() {
        static ExpectedSizeRegistry reg;
        return reg;
    }

    // Load from devices.json (call once at startup)
    bool loadDevices(const std::string& path = "devices.json") {
        std::ifstream file(path);
        if (!file.is_open()) {
            MLOG_WARN("ExpectedSizeRegistry", "devices.json not found: %s", path.c_str());
            return false;
        }

#if MIRAGE_HAS_JSON
        try {
            nlohmann::json j = nlohmann::json::parse(file);
            if (!j.contains("devices") || !j["devices"].is_array()) {
                MLOG_WARN("ExpectedSizeRegistry", "Invalid devices.json format");
                return false;
            }

            devices_.clear();
            for (const auto& dev : j["devices"]) {
                ExpectedDeviceSpec spec;
                spec.hardware_id = dev.value("hardware_id", "");
                spec.screen_width = dev.value("screen_width", 0);
                spec.screen_height = dev.value("screen_height", 0);
                spec.screen_density = dev.value("screen_density", 0);
                spec.tcp_port = dev.value("tcp_port", 0);

                if (!spec.hardware_id.empty() && spec.screen_width > 0 && spec.screen_height > 0) {
                    devices_[spec.hardware_id] = spec;
                    MLOG_INFO("ExpectedSizeRegistry", "Loaded: %s -> %dx%d",
                              spec.hardware_id.c_str(), spec.screen_width, spec.screen_height);
                }
            }
            MLOG_INFO("ExpectedSizeRegistry", "Loaded %zu devices", devices_.size());
            return true;
        } catch (const nlohmann::json::exception& e) {
            MLOG_ERROR("ExpectedSizeRegistry", "JSON parse error: %s", e.what());
            return false;
        }
#else
        MLOG_WARN("ExpectedSizeRegistry", "nlohmann/json not available");
        return false;
#endif
    }

    // Get expected resolution for device (returns false if unknown)
    bool getExpectedSize(const std::string& hardware_id, int& out_w, int& out_h) const {
        auto it = devices_.find(hardware_id);
        if (it != devices_.end()) {
            out_w = it->second.screen_width;
            out_h = it->second.screen_height;
            return true;
        }
        return false;
    }

    bool getTcpPort(const std::string& hardware_id, int& out_port) const {
        auto it = devices_.find(hardware_id);
        if (it != devices_.end() && it->second.tcp_port > 0) {
            out_port = it->second.tcp_port;
            return true;
        }
        return false;
    }

    const std::map<std::string, ExpectedDeviceSpec>& allDevices() const { return devices_; }


private:
    ExpectedSizeRegistry() = default;
    std::map<std::string, ExpectedDeviceSpec> devices_;
};

} // namespace config
} // namespace mirage


