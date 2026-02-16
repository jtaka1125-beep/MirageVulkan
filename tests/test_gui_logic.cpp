// =============================================================================
// MirageVulkan - GUI Logic Unit Tests
// =============================================================================
// Tests GUI-related logic that doesn't require Vulkan/window initialization:
//   - GuiConfig validation
//   - Layout calculations
//   - DeviceInfo state management
//   - Input threshold calculations
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <cmath>

// =============================================================================
// Re-declare GUI types for testing (avoiding Vulkan dependencies)
// =============================================================================
namespace test {

enum class DeviceStatus {
    Disconnected,
    Idle,
    AndroidActive,
    AIActive,
    Stuck,
    Error
};

struct DeviceInfo {
    std::string id;
    std::string name;
    DeviceStatus status = DeviceStatus::Disconnected;
    int aoa_version = -1;
    float fps = 0;
    float latency_ms = 0;
    float bandwidth_mbps = 0;
    uint64_t frame_count = 0;
    int texture_width = 0;
    int texture_height = 0;
    uint64_t last_frame_time = 0;
    uint64_t status_changed_at = 0;
};

namespace constants {
    constexpr float MIN_SWIPE_DISTANCE = 20.0f;
    constexpr int MIN_SWIPE_DURATION_MS = 100;
    constexpr int MAX_SWIPE_DURATION_MS = 1000;
    constexpr float SWIPE_DURATION_FACTOR = 0.5f;
}

struct GuiConfig {
    int window_width = 1920;
    int window_height = 1080;
    bool vsync = true;
    float left_ratio = 0.4f;
    float center_ratio = 0.3f;
    float right_ratio = 0.3f;
    uint32_t color_disconnected = 0xFF404040;
    uint32_t color_idle = 0xFF808080;
    uint32_t color_android_active = 0xFF00FF00;
    uint32_t color_ai_active = 0xFFFF8800;
    uint32_t color_stuck = 0xFF0000FF;
    uint32_t color_error = 0xFF00FFFF;
    float overlay_alpha = 0.6f;
    bool show_fps = true;
    bool show_latency = true;
    bool show_match_boxes = true;
    bool show_match_labels = true;
    int sub_grid_padding = 4;
    int sub_border_width = 3;
    int max_log_entries = 1000;
    bool auto_scroll_log = true;
};

// Layout calculation functions
struct LayoutRegion {
    int x, y, width, height;
};

LayoutRegion calculateLeftPanel(const GuiConfig& config) {
    int width = static_cast<int>(config.window_width * config.left_ratio);
    return {0, 0, width, config.window_height};
}

LayoutRegion calculateCenterPanel(const GuiConfig& config) {
    int left_width = static_cast<int>(config.window_width * config.left_ratio);
    int center_width = static_cast<int>(config.window_width * config.center_ratio);
    return {left_width, 0, center_width, config.window_height};
}

LayoutRegion calculateRightPanel(const GuiConfig& config) {
    int left_width = static_cast<int>(config.window_width * config.left_ratio);
    int center_width = static_cast<int>(config.window_width * config.center_ratio);
    int right_width = config.window_width - left_width - center_width;
    return {left_width + center_width, 0, right_width, config.window_height};
}

// Sub-grid layout for multiple devices
struct SubGridLayout {
    int rows;
    int cols;
    int cell_width;
    int cell_height;
};

SubGridLayout calculateSubGrid(int panel_width, int panel_height, int device_count, int padding) {
    SubGridLayout layout{1, 1, panel_width, panel_height};

    if (device_count <= 1) {
        layout.rows = 1;
        layout.cols = 1;
    } else if (device_count <= 4) {
        layout.rows = 2;
        layout.cols = 2;
    } else if (device_count <= 9) {
        layout.rows = 3;
        layout.cols = 3;
    } else {
        // 4x4 grid for more devices
        layout.rows = 4;
        layout.cols = 4;
    }

    layout.cell_width = (panel_width - padding * (layout.cols + 1)) / layout.cols;
    layout.cell_height = (panel_height - padding * (layout.rows + 1)) / layout.rows;

    return layout;
}

// Swipe duration calculation
int calculateSwipeDuration(float distance) {
    if (distance < constants::MIN_SWIPE_DISTANCE) {
        return 0;  // Not a swipe
    }
    int duration = static_cast<int>(distance * constants::SWIPE_DURATION_FACTOR);
    duration = std::max(duration, constants::MIN_SWIPE_DURATION_MS);
    duration = std::min(duration, constants::MAX_SWIPE_DURATION_MS);
    return duration;
}

// Status color lookup
uint32_t getStatusColor(const GuiConfig& config, DeviceStatus status) {
    switch (status) {
        case DeviceStatus::Disconnected: return config.color_disconnected;
        case DeviceStatus::Idle: return config.color_idle;
        case DeviceStatus::AndroidActive: return config.color_android_active;
        case DeviceStatus::AIActive: return config.color_ai_active;
        case DeviceStatus::Stuck: return config.color_stuck;
        case DeviceStatus::Error: return config.color_error;
        default: return config.color_disconnected;
    }
}

} // namespace test

// =============================================================================
// Test: GuiConfig Defaults
// =============================================================================
TEST(GuiConfigTest, DefaultValues) {
    test::GuiConfig config;

    EXPECT_EQ(config.window_width, 1920);
    EXPECT_EQ(config.window_height, 1080);
    EXPECT_TRUE(config.vsync);
    EXPECT_FLOAT_EQ(config.left_ratio, 0.4f);
    EXPECT_FLOAT_EQ(config.center_ratio, 0.3f);
    EXPECT_FLOAT_EQ(config.right_ratio, 0.3f);
}

TEST(GuiConfigTest, RatiosSumToOne) {
    test::GuiConfig config;
    float sum = config.left_ratio + config.center_ratio + config.right_ratio;
    EXPECT_FLOAT_EQ(sum, 1.0f);
}

TEST(GuiConfigTest, ColorValues) {
    test::GuiConfig config;

    // Green for Android active (ABGR: 0xFF00FF00)
    EXPECT_EQ(config.color_android_active, 0xFF00FF00u);
    // Red for stuck (ABGR: 0xFF0000FF)
    EXPECT_EQ(config.color_stuck, 0xFF0000FFu);
}

// =============================================================================
// Test: Layout Calculations
// =============================================================================
TEST(LayoutTest, LeftPanelAt1920x1080) {
    test::GuiConfig config;
    config.window_width = 1920;
    config.window_height = 1080;

    auto region = test::calculateLeftPanel(config);

    EXPECT_EQ(region.x, 0);
    EXPECT_EQ(region.y, 0);
    EXPECT_EQ(region.width, 768);  // 1920 * 0.4
    EXPECT_EQ(region.height, 1080);
}

TEST(LayoutTest, CenterPanelAt1920x1080) {
    test::GuiConfig config;
    config.window_width = 1920;
    config.window_height = 1080;

    auto region = test::calculateCenterPanel(config);

    EXPECT_EQ(region.x, 768);
    EXPECT_EQ(region.width, 576);  // 1920 * 0.3
    EXPECT_EQ(region.height, 1080);
}

TEST(LayoutTest, RightPanelAt1920x1080) {
    test::GuiConfig config;
    config.window_width = 1920;
    config.window_height = 1080;

    auto region = test::calculateRightPanel(config);

    EXPECT_EQ(region.x, 1344);  // 768 + 576
    EXPECT_EQ(region.width, 576);
    EXPECT_EQ(region.height, 1080);
}

TEST(LayoutTest, PanelsCoverFullWidth) {
    test::GuiConfig config;
    config.window_width = 1920;

    auto left = test::calculateLeftPanel(config);
    auto center = test::calculateCenterPanel(config);
    auto right = test::calculateRightPanel(config);

    EXPECT_EQ(left.width + center.width + right.width, 1920);
}

TEST(LayoutTest, DifferentResolution) {
    test::GuiConfig config;
    config.window_width = 2560;
    config.window_height = 1440;

    auto left = test::calculateLeftPanel(config);
    auto center = test::calculateCenterPanel(config);
    auto right = test::calculateRightPanel(config);

    EXPECT_EQ(left.width, 1024);   // 2560 * 0.4
    EXPECT_EQ(center.width, 768); // 2560 * 0.3
    EXPECT_EQ(left.height, 1440);
}

// =============================================================================
// Test: Sub-Grid Layout
// =============================================================================
TEST(SubGridTest, SingleDevice) {
    auto grid = test::calculateSubGrid(576, 1080, 1, 4);

    EXPECT_EQ(grid.rows, 1);
    EXPECT_EQ(grid.cols, 1);
}

TEST(SubGridTest, TwoToFourDevices) {
    auto grid = test::calculateSubGrid(576, 1080, 4, 4);

    EXPECT_EQ(grid.rows, 2);
    EXPECT_EQ(grid.cols, 2);
}

TEST(SubGridTest, FiveToNineDevices) {
    auto grid = test::calculateSubGrid(576, 1080, 9, 4);

    EXPECT_EQ(grid.rows, 3);
    EXPECT_EQ(grid.cols, 3);
}

TEST(SubGridTest, MoreThanNineDevices) {
    auto grid = test::calculateSubGrid(576, 1080, 16, 4);

    EXPECT_EQ(grid.rows, 4);
    EXPECT_EQ(grid.cols, 4);
}

TEST(SubGridTest, CellSizeWithPadding) {
    int panel_width = 576;
    int panel_height = 1080;
    int padding = 4;

    auto grid = test::calculateSubGrid(panel_width, panel_height, 4, padding);

    // 2x2 grid: (576 - 4*3) / 2 = 282
    int expected_width = (panel_width - padding * 3) / 2;
    EXPECT_EQ(grid.cell_width, expected_width);
}

// =============================================================================
// Test: Swipe Duration Calculation
// =============================================================================
TEST(SwipeTest, BelowMinimumDistance) {
    float distance = 10.0f;  // Below MIN_SWIPE_DISTANCE (20.0)
    int duration = test::calculateSwipeDuration(distance);

    EXPECT_EQ(duration, 0);  // Not a swipe
}

TEST(SwipeTest, MinimumDuration) {
    float distance = 50.0f;  // 50 * 0.5 = 25ms, but minimum is 100ms
    int duration = test::calculateSwipeDuration(distance);

    EXPECT_EQ(duration, test::constants::MIN_SWIPE_DURATION_MS);
}

TEST(SwipeTest, NormalSwipe) {
    float distance = 400.0f;  // 400 * 0.5 = 200ms
    int duration = test::calculateSwipeDuration(distance);

    EXPECT_EQ(duration, 200);
}

TEST(SwipeTest, MaximumDuration) {
    float distance = 5000.0f;  // 5000 * 0.5 = 2500ms, but max is 1000ms
    int duration = test::calculateSwipeDuration(distance);

    EXPECT_EQ(duration, test::constants::MAX_SWIPE_DURATION_MS);
}

// =============================================================================
// Test: Device Status Colors
// =============================================================================
TEST(StatusColorTest, AllStatusesHaveColors) {
    test::GuiConfig config;

    EXPECT_NE(test::getStatusColor(config, test::DeviceStatus::Disconnected), 0u);
    EXPECT_NE(test::getStatusColor(config, test::DeviceStatus::Idle), 0u);
    EXPECT_NE(test::getStatusColor(config, test::DeviceStatus::AndroidActive), 0u);
    EXPECT_NE(test::getStatusColor(config, test::DeviceStatus::AIActive), 0u);
    EXPECT_NE(test::getStatusColor(config, test::DeviceStatus::Stuck), 0u);
    EXPECT_NE(test::getStatusColor(config, test::DeviceStatus::Error), 0u);
}

TEST(StatusColorTest, UniqueColors) {
    test::GuiConfig config;

    uint32_t colors[] = {
        test::getStatusColor(config, test::DeviceStatus::Disconnected),
        test::getStatusColor(config, test::DeviceStatus::Idle),
        test::getStatusColor(config, test::DeviceStatus::AndroidActive),
        test::getStatusColor(config, test::DeviceStatus::AIActive),
        test::getStatusColor(config, test::DeviceStatus::Stuck),
        test::getStatusColor(config, test::DeviceStatus::Error)
    };

    // All colors should be unique
    for (int i = 0; i < 6; i++) {
        for (int j = i + 1; j < 6; j++) {
            EXPECT_NE(colors[i], colors[j])
                << "Status colors at index " << i << " and " << j << " are identical";
        }
    }
}

// =============================================================================
// Test: DeviceInfo State Management
// =============================================================================
TEST(DeviceInfoTest, DefaultState) {
    test::DeviceInfo device;

    EXPECT_EQ(device.status, test::DeviceStatus::Disconnected);
    EXPECT_EQ(device.aoa_version, -1);
    EXPECT_FLOAT_EQ(device.fps, 0.0f);
    EXPECT_EQ(device.frame_count, 0u);
}

TEST(DeviceInfoTest, StateTransitions) {
    test::DeviceInfo device;
    device.id = "device_001";
    device.name = "Test Device";

    // Initial state
    EXPECT_EQ(device.status, test::DeviceStatus::Disconnected);

    // Connect
    device.status = test::DeviceStatus::Idle;
    EXPECT_EQ(device.status, test::DeviceStatus::Idle);

    // Start processing
    device.status = test::DeviceStatus::AndroidActive;
    EXPECT_EQ(device.status, test::DeviceStatus::AndroidActive);

    // AI processing
    device.status = test::DeviceStatus::AIActive;
    EXPECT_EQ(device.status, test::DeviceStatus::AIActive);

    // Stuck detection
    device.status = test::DeviceStatus::Stuck;
    EXPECT_EQ(device.status, test::DeviceStatus::Stuck);

    // Error
    device.status = test::DeviceStatus::Error;
    EXPECT_EQ(device.status, test::DeviceStatus::Error);
}

// =============================================================================
// Test: Device Registry (map-based)
// =============================================================================
TEST(DeviceRegistryTest, AddAndRemove) {
    std::map<std::string, test::DeviceInfo> devices;

    // Add device
    test::DeviceInfo dev1;
    dev1.id = "dev_001";
    dev1.name = "Device 1";
    devices[dev1.id] = dev1;

    EXPECT_EQ(devices.size(), 1u);
    EXPECT_TRUE(devices.count("dev_001") > 0);

    // Add another
    test::DeviceInfo dev2;
    dev2.id = "dev_002";
    dev2.name = "Device 2";
    devices[dev2.id] = dev2;

    EXPECT_EQ(devices.size(), 2u);

    // Remove
    devices.erase("dev_001");
    EXPECT_EQ(devices.size(), 1u);
    EXPECT_FALSE(devices.count("dev_001") > 0);
}

TEST(DeviceRegistryTest, UpdateExisting) {
    std::map<std::string, test::DeviceInfo> devices;

    test::DeviceInfo dev;
    dev.id = "dev_001";
    dev.fps = 30.0f;
    devices[dev.id] = dev;

    // Update FPS
    devices["dev_001"].fps = 60.0f;

    EXPECT_FLOAT_EQ(devices["dev_001"].fps, 60.0f);
}

// =============================================================================
// Test: Aspect Ratio Preservation
// =============================================================================
TEST(AspectRatioTest, FitToPanel) {
    // Device frame: 1200x2000 (portrait, aspect = 0.6)
    int frame_w = 1200;
    int frame_h = 2000;

    // Panel: 576x1080 (aspect = 0.533)
    int panel_w = 576;
    int panel_h = 1080;

    float frame_aspect = static_cast<float>(frame_w) / frame_h;  // 0.6
    float panel_aspect = static_cast<float>(panel_w) / panel_h;  // 0.533

    int display_w, display_h;

    if (frame_aspect < panel_aspect) {
        // Frame is taller (narrower) - fit to height
        display_h = panel_h;
        display_w = static_cast<int>(display_h * frame_aspect);
    } else {
        // Frame is wider - fit to width
        display_w = panel_w;
        display_h = static_cast<int>(display_w / frame_aspect);
    }

    // frame_aspect (0.6) > panel_aspect (0.533), so fit to width
    EXPECT_EQ(display_w, 576);
    // 576 / 0.6 = 960, but integer truncation may give 959
    EXPECT_NEAR(display_h, 960, 1);

    // Verify aspect ratio preserved (within rounding error)
    float result_aspect = static_cast<float>(display_w) / display_h;
    EXPECT_NEAR(result_aspect, frame_aspect, 0.01f);
}

// =============================================================================
// Test: FPS Calculation
// =============================================================================
TEST(FpsCalculationTest, FrameInterval) {
    uint64_t last_time = 0;
    uint64_t current_time = 33333;  // ~33ms for 30fps

    float fps = 1000000.0f / (current_time - last_time);

    EXPECT_NEAR(fps, 30.0f, 1.0f);
}

TEST(FpsCalculationTest, Smoothing) {
    // Exponential moving average
    float alpha = 0.1f;
    float current_fps = 30.0f;
    float instant_fps = 60.0f;

    float smoothed_fps = alpha * instant_fps + (1.0f - alpha) * current_fps;

    EXPECT_NEAR(smoothed_fps, 33.0f, 0.1f);  // 0.1 * 60 + 0.9 * 30 = 33
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
