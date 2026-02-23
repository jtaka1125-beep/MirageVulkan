// =============================================================================
// Unit tests for config_loader.hpp
// Tests: defaults, LogConfig, extract helpers, file loading, singleton
// =============================================================================
#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include "config_loader.hpp"

using namespace mirage::config;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void writeTmpJson(const char* path, const char* content) {
    std::ofstream f(path);
    f << content;
}

// ---------------------------------------------------------------------------
// C-1: AppConfig has all 6 sub-config types with correct defaults
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, DefaultValues) {
    AppConfig cfg;
    EXPECT_EQ(cfg.network.pc_ip,                  "192.168.0.7");
    EXPECT_EQ(cfg.network.video_base_port,         60000);
    EXPECT_EQ(cfg.network.command_base_port,       50000);
    EXPECT_EQ(cfg.network.tcp_command_port,        50100);
    EXPECT_EQ(cfg.usb_tether.android_ip,           "192.168.42.129");
    EXPECT_EQ(cfg.gui.window_width,                1920);
    EXPECT_EQ(cfg.gui.window_height,               1080);
    EXPECT_TRUE(cfg.gui.vsync);
    EXPECT_TRUE(cfg.ai.enabled);
    EXPECT_EQ(cfg.ai.templates_dir,                "templates");
    EXPECT_FLOAT_EQ(cfg.ai.default_threshold,      0.80f);
    EXPECT_FALSE(cfg.ocr.enabled);
    EXPECT_EQ(cfg.ocr.language,                    "eng+jpn");
    EXPECT_EQ(cfg.log.log_path,                    "mirage_gui.log");
}

// ---------------------------------------------------------------------------
// C-2: LogConfig default is "mirage_gui.log"
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, LogConfigDefault) {
    LogConfig lc;
    EXPECT_EQ(lc.log_path, "mirage_gui.log");
}

// ---------------------------------------------------------------------------
// C-3: loadConfig with missing file returns all defaults
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, LoadConfigMissingFileReturnsDefaults) {
    AppConfig cfg = loadConfig("__nonexistent_config_xyz.json", true);
    EXPECT_EQ(cfg.network.pc_ip, "192.168.0.7");
    EXPECT_EQ(cfg.log.log_path,  "mirage_gui.log");
    EXPECT_EQ(cfg.ai.templates_dir, "templates");
}

// ---------------------------------------------------------------------------
// C-4: extractJsonString parses key correctly
// ---------------------------------------------------------------------------
#if !MIRAGE_HAS_JSON
TEST(ConfigLoaderTest, ExtractJsonString) {
    std::string json = R"({"key": "hello_world"})";
    EXPECT_EQ(extractJsonString(json, "key"), "hello_world");
}

TEST(ConfigLoaderTest, ExtractJsonStringMissing) {
    std::string json = R"({"other": "value"})";
    EXPECT_EQ(extractJsonString(json, "key"), "");
}

// ---------------------------------------------------------------------------
// C-5: extractJsonInt
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, ExtractJsonInt) {
    std::string json = R"({"port": 60000})";
    EXPECT_EQ(extractJsonInt(json, "port", 0), 60000);
    EXPECT_EQ(extractJsonInt(json, "missing", 99), 99);
}

// ---------------------------------------------------------------------------
// C-6: extractJsonFloat
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, ExtractJsonFloat) {
    std::string json = R"({"thresh": 0.75})";
    EXPECT_FLOAT_EQ(extractJsonFloat(json, "thresh", 0.0f), 0.75f);
    EXPECT_FLOAT_EQ(extractJsonFloat(json, "missing", 1.0f), 1.0f);
}

// ---------------------------------------------------------------------------
// C-7: extractJsonBool
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, ExtractJsonBool) {
    std::string json_true  = R"({"flag": true})";
    std::string json_false = R"({"flag": false})";
    EXPECT_TRUE(extractJsonBool(json_true,  "flag", false));
    EXPECT_FALSE(extractJsonBool(json_false, "flag", true));
    EXPECT_TRUE(extractJsonBool(json_true,  "missing", true));
}
#endif  // !MIRAGE_HAS_JSON

// ---------------------------------------------------------------------------
// C-8: loadConfig parses a temp JSON file correctly
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, LoadConfigFromFile) {
    const char* tmp = "__test_config_tmp.json";
    writeTmpJson(tmp, R"({
        "network": { "pc_ip": "10.0.0.1", "video_base_port": 61000 },
        "ai":      { "templates_dir": "my_templates", "default_threshold": 0.90 },
        "log":     { "log_path": "custom.log" }
    })");

    AppConfig cfg = loadConfig(tmp, true);
    std::remove(tmp);

    EXPECT_EQ(cfg.network.pc_ip,        "10.0.0.1");
    EXPECT_EQ(cfg.network.video_base_port, 61000);
    EXPECT_EQ(cfg.ai.templates_dir,     "my_templates");
    EXPECT_NEAR(cfg.ai.default_threshold, 0.90f, 0.001f);
    EXPECT_EQ(cfg.log.log_path,         "custom.log");
    // Unspecified fields retain defaults
    EXPECT_EQ(cfg.network.command_base_port, 50000);
    EXPECT_EQ(cfg.ocr.language,         "eng+jpn");
}

// ---------------------------------------------------------------------------
// C-9: loadConfig log_path defaults to "mirage_gui.log" when key absent
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, LoadConfigLogPathDefaultWhenAbsent) {
    const char* tmp = "__test_config_tmp2.json";
    writeTmpJson(tmp, R"({ "network": { "pc_ip": "192.168.0.7" } })");

    AppConfig cfg = loadConfig(tmp, true);
    std::remove(tmp);

    EXPECT_EQ(cfg.log.log_path, "mirage_gui.log");
}

// ---------------------------------------------------------------------------
// C-10: getConfig() returns a stable singleton reference
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, GetConfigSingleton) {
    AppConfig& a = getConfig();
    AppConfig& b = getConfig();
    EXPECT_EQ(&a, &b) << "getConfig() must return the same instance";
    // Verify it has defaults (or whatever file is loaded in test env)
    EXPECT_FALSE(a.network.pc_ip.empty());
    EXPECT_FALSE(a.log.log_path.empty());
}

// ---------------------------------------------------------------------------
// C-11: ExpectedSizeRegistry instance is a stable singleton
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, ExpectedSizeRegistrySingleton) {
    auto& r1 = ExpectedSizeRegistry::instance();
    auto& r2 = ExpectedSizeRegistry::instance();
    EXPECT_EQ(&r1, &r2);
}

// ---------------------------------------------------------------------------
// C-12: ExpectedSizeRegistry returns false for unknown device
// ---------------------------------------------------------------------------
TEST(ConfigLoaderTest, ExpectedSizeRegistryUnknownDevice) {
    auto& reg = ExpectedSizeRegistry::instance();
    int w = 0, h = 0;
    EXPECT_FALSE(reg.getExpectedSize("unknown_hw_id_xyz", w, h));
    EXPECT_EQ(w, 0);
    EXPECT_EQ(h, 0);
}
