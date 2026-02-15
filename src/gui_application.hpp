// =============================================================================
// MirageSystem v2 - GUI Application
// =============================================================================
// ImGui + Vulkan based control interface
// 
// Layout (FHD 1920x1080):
//   Left (768px):   Controls, Learning toggle, Logs
//   Center (576px): Main device view with touch interaction
//   Right (576px):  Sub device grid (1/4/9 adaptive)
// =============================================================================

#pragma once


// Vulkan backend
#include "vulkan/vulkan_context.hpp"
#include "vulkan/vulkan_swapchain.hpp"
#include "vulkan/vulkan_texture.hpp"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <chrono>
#include <deque>
#include <mutex>
#include <atomic>

// Forward declarations
struct ImGuiContext;
struct ImDrawData;

namespace gui {
    class AdbDeviceManager;
}

namespace mirage::gui {


// =============================================================================
// Device Status
// =============================================================================
enum class DeviceStatus {
    Disconnected,   // \u672a\u63a5\u7d9a
    Idle,           // \u63a5\u7d9a\u6e08\u307f\u3001\u5f85\u6a5f\u4e2d (\u767d/\u30b0\u30ec\u30fc)
    AndroidActive,  // Android\u5074\u51e6\u7406\u4e2d (\u7dd1)
    AIActive,       // AI\u5224\u5b9a\u4e2d (\u9752)
    Stuck,          // \u8a70\u307e\u308a\u691c\u51fa (\u8d64)
    Error           // \u30a8\u30e9\u30fc (\u9ec4)
};

struct DeviceInfo {
    std::string id;
    std::string name;
    DeviceStatus status = DeviceStatus::Disconnected;
    
    // AOA
    int aoa_version = -1;           // -1=unchecked, 0=not supported, 1=v1, 2=v2(HID)

    // Statistics
    float fps = 0;
    float latency_ms = 0;
    float bandwidth_mbps = 0;
    uint64_t frame_count = 0;
    
    // Current frame texture
    VkDescriptorSet vk_texture_ds = VK_NULL_HANDLE;
    std::shared_ptr<mirage::vk::VulkanTexture> vk_texture;
    int texture_width = 0;
    int texture_height = 0;
    
    // Matching results overlay
    struct MatchOverlay {
        std::string template_id;
        std::string label;
        int x, y, w, h;
        float score;
        uint32_t color;  // ABGR
    };
    std::vector<MatchOverlay> overlays;
    
    // Timing
    uint64_t last_frame_time = 0;
    uint64_t status_changed_at = 0;
};

// =============================================================================
// Learning Mode Data
// =============================================================================
struct LearningClickData {
    int click_x;
    int click_y;
    uint64_t timestamp;
    
    // Captured context
    std::vector<DeviceInfo::MatchOverlay> visible_elements;
    std::string scene_name;
    
    // Relative positions to nearby elements
    struct RelativePosition {
        std::string element_id;
        int dx, dy;  // offset from element center
        float distance;
    };
    std::vector<RelativePosition> relative_positions;
};

struct LearningSession {
    bool active = false;
    std::vector<LearningClickData> collected_clicks;
    std::string session_name;
    uint64_t started_at = 0;
};

// =============================================================================
// Log Entry
// =============================================================================
struct LogEntry {
    enum class Level { Debug, Info, Warning, Error };
    
    Level level;
    std::string message;
    std::string source;  // device_id or "system"
    uint64_t timestamp;
    
    uint32_t getColor() const {
        switch (level) {
            case Level::Debug:   return 0xFF888888;  // Gray
            case Level::Info:    return 0xFFFFFFFF;  // White
            case Level::Warning: return 0xFF00FFFF;  // Yellow
            case Level::Error:   return 0xFF0000FF;  // Red
            default:             return 0xFFFFFFFF;
        }
    }
};

// =============================================================================
// GUI Configuration
// =============================================================================

// Layout constants (shared between input and render)
namespace layout_constants {
    constexpr float PANEL_HEADER_HEIGHT = 28.0f;  // ImGui header height (unified)
    constexpr float TEST_TAP_X = 540;   // Test tap X coordinate (center of 1080p)
    constexpr float TEST_TAP_Y = 960;   // Test tap Y coordinate
    constexpr int   DEFAULT_SCREEN_W = 1080;
    constexpr int   DEFAULT_SCREEN_H = 1920;

    // Input thresholds
    constexpr float MIN_SWIPE_DISTANCE = 20.0f;    // Minimum drag distance to be considered a swipe
    constexpr int   MIN_SWIPE_DURATION_MS = 100;   // Minimum swipe duration
    constexpr int   MAX_SWIPE_DURATION_MS = 1000;  // Maximum swipe duration
    constexpr float SWIPE_DURATION_FACTOR = 0.5f;  // Factor to convert distance to duration
}

struct GuiConfig {
    // Window
    int window_width = 1920;
    int window_height = 1080;
    bool vsync = true;
    
    // Layout ratios (4:3:3)
    float left_ratio = 0.4f;    // 768px at 1920
    float center_ratio = 0.3f;  // 576px
    float right_ratio = 0.3f;   // 576px
    
    // Colors (ABGR format)
    uint32_t color_disconnected = 0xFF404040;
    uint32_t color_idle = 0xFF808080;
    uint32_t color_android_active = 0xFF00FF00;  // Green
    uint32_t color_ai_active = 0xFFFF8800;       // Blue (BGR)
    uint32_t color_stuck = 0xFF0000FF;           // Red
    uint32_t color_error = 0xFF00FFFF;           // Yellow
    
    // Overlay
    float overlay_alpha = 0.6f;
    bool show_fps = true;
    bool show_latency = true;
    bool show_match_boxes = true;
    bool show_match_labels = true;
    
    // Sub-screen grid
    int sub_grid_padding = 4;
    int sub_border_width = 3;
    
    // Log
    int max_log_entries = 1000;
    bool auto_scroll_log = true;
};

// =============================================================================
// GUI Application
// =============================================================================
class GuiApplication {
public:
    // Callbacks
    using TapCallback = std::function<void(const std::string& device_id, int x, int y)>;
    using SwipeCallback = std::function<void(const std::string& device_id,
                                              int x1, int y1, int x2, int y2, int duration_ms)>;
    using DeviceSelectCallback = std::function<void(const std::string& device_id)>;
    using LearningDataCallback = std::function<void(const LearningClickData& data)>;
    using StartMirroringCallback = std::function<void()>;
    
    GuiApplication();
    ~GuiApplication();
    
    // Initialization
    bool initialize(HWND hwnd, const GuiConfig& config = GuiConfig{});
    void shutdown();
    
    // Main loop
    void beginFrame();
    void render();
    void endFrame();
    
    // Device management
    void addDevice(const std::string& id, const std::string& name);
    void removeDevice(const std::string& id);
    void setMainDevice(const std::string& id);
    std::string getMainDevice() const { return main_device_id_; }
    
    // Device updates
    void updateDeviceStatus(const std::string& id, DeviceStatus status);
    void updateDeviceFrame(const std::string& id, 
                           const uint8_t* rgba_data, int width, int height);
    void updateDeviceOverlays(const std::string& id,
                              const std::vector<DeviceInfo::MatchOverlay>& overlays);
    void updateDeviceStats(const std::string& id, float fps, float latency_ms, float bandwidth_mbps);
    
    // Thread-safe frame queue (call from any thread)
    // This copies data to queue, main thread processes with processPendingFrames()
    void queueFrame(const std::string& id, const uint8_t* rgba_data, int width, int height);
    
    // Process pending frames - MUST be called from main thread only
    void processPendingFrames();
    
    // Logging
    void log(LogEntry::Level level, const std::string& message, 
             const std::string& source = "system");
    void logDebug(const std::string& msg, const std::string& src = "system") {
        log(LogEntry::Level::Debug, msg, src);
    }
    void logInfo(const std::string& msg, const std::string& src = "system") {
        log(LogEntry::Level::Info, msg, src);
    }
    void logWarning(const std::string& msg, const std::string& src = "system") {
        log(LogEntry::Level::Warning, msg, src);
    }
    void logError(const std::string& msg, const std::string& src = "system") {
        log(LogEntry::Level::Error, msg, src);
    }
    
    // Callbacks
    void setTapCallback(TapCallback cb) { tap_callback_ = std::move(cb); }
    void setSwipeCallback(SwipeCallback cb) { swipe_callback_ = std::move(cb); }
    void setDeviceSelectCallback(DeviceSelectCallback cb) { device_select_callback_ = std::move(cb); }
    void setLearningDataCallback(LearningDataCallback cb) { learning_data_callback_ = std::move(cb); }
    void setStartMirroringCallback(StartMirroringCallback cb) { start_mirroring_callback_ = std::move(cb); }
    
    // Learning mode
    bool isLearningMode() const { return learning_session_.active; }
    void startLearningSession(const std::string& name);
    void stopLearningSession();
    void exportLearningData();
    const LearningSession& getLearningSession() const { return learning_session_; }
    
    // Input handling (call from WndProc)
    void onMouseMove(int x, int y);
    void onMouseDown(int button, int x, int y);
    void onMouseUp(int button, int x, int y);
    void onMouseDoubleClick(int button, int x, int y);
    void onKeyDown(int vkey);
    void onKeyUp(int vkey);
    void onResize(int width, int height);
    
    // State
    bool isRunning() const { return running_; }
    mirage::vk::VulkanContext* vulkanContext() { return vk_context_.get(); }
    void requestQuit() { running_ = false; }

    // ADB Device Manager integration
    void setAdbDeviceManager(::gui::AdbDeviceManager* manager) { adb_manager_ = manager; }

    // Screenshot capture and display
    void captureScreenshot(const std::string& device_id);
    bool hasScreenshot() const { return !screenshot_data_.empty(); }
    void clearScreenshot() { screenshot_data_.clear(); screenshot_device_id_.clear(); }
    void showScreenshotPopup(bool show) { show_screenshot_popup_ = show; }
    
private:
    // Vulkan backend
    bool createVulkanResources(HWND hwnd);
    void cleanupVulkanResources();
    bool setupImGuiVulkan(HWND hwnd);
    void vulkanBeginFrame();
    void vulkanEndFrame();
    
    // Rendering
    void renderLeftPanel();
    void renderCenterPanel();
    void renderRightPanel();
    void renderDeviceView(DeviceInfo& device, float x, float y, float w, float h, 
                          bool is_main, bool draw_border);
    void renderOverlays(DeviceInfo& device, float view_x, float view_y, 
                        float view_w, float view_h);
    void renderStatusBorder(float x, float y, float w, float h,
                            DeviceStatus status, int border_width);
    void renderScreenshotPopup();
    
    // Layout calculation
    struct LayoutRects {
        float left_x, left_w;
        float center_x, center_w;
        float right_x, right_w;
        float height;
    };
    LayoutRects calculateLayout() const;
    
    // Sub-screen grid
    struct SubGridLayout {
        int cols, rows;
        float cell_w, cell_h;
    };
    SubGridLayout calculateSubGrid(int device_count, float panel_w, float panel_h) const;
    
    // Input processing
    void processMainViewClick(int local_x, int local_y, bool is_double);
    void processSubViewClick(int panel_x, int panel_y, bool is_double);
    void processSwipe(int x1, int y1, int x2, int y2);
    std::pair<int, int> screenToDeviceCoords(const DeviceInfo& device,
                                              float view_x, float view_y,
                                              float view_w, float view_h,
                                              int screen_x, int screen_y) const;
    
    // Learning mode
    LearningClickData collectLearningData(const DeviceInfo& device, int x, int y);
    
    // Utility
    uint32_t getStatusColor(DeviceStatus status) const;
    std::string getStatusText(DeviceStatus status) const;
    uint64_t getCurrentTimeMs() const;
    
private:
    GuiConfig config_;
    bool running_ = true;
    bool imgui_initialized_ = false;
    // Vulkan backend
    std::unique_ptr<mirage::vk::VulkanContext>   vk_context_;
    std::unique_ptr<mirage::vk::VulkanSwapchain> vk_swapchain_;
    VkDescriptorPool      vk_descriptor_pool_ = VK_NULL_HANDLE;
    VkCommandPool         vk_command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> vk_command_buffers_;
    std::vector<VkSemaphore>     vk_image_available_;
    std::vector<VkSemaphore>     vk_render_finished_;
    std::vector<VkFence>         vk_in_flight_;
    uint32_t              vk_current_frame_ = 0;
    static constexpr uint32_t VK_MAX_FRAMES_IN_FLIGHT = 2;
    uint32_t              vk_current_image_index_ = 0;
    bool vulkan_initialized_ = false;
    
    // Devices
    std::map<std::string, DeviceInfo> devices_;
    std::string main_device_id_;
    std::vector<std::string> device_order_;  // for consistent sub-screen ordering
    mutable std::mutex devices_mutex_;
    
    // === Thread-safe frame queue ===
    struct PendingFrame {
        std::vector<uint8_t> rgba_data;
        int width = 0;
        int height = 0;
    };
    // Per-device latest frame only (older frames auto-discarded on overwrite)
    std::map<std::string, PendingFrame> pending_frames_;
    mutable std::mutex pending_frames_mutex_;
    
    // Logs
    std::deque<LogEntry> logs_;
    mutable std::mutex logs_mutex_;
    
    // Learning (protected by learning_mutex_ for thread safety)
    LearningSession learning_session_;
    mutable std::mutex learning_mutex_;
    
    // Input state
    int mouse_x_ = 0, mouse_y_ = 0;
    bool mouse_down_[3] = {false, false, false};
    int drag_start_x_ = 0, drag_start_y_ = 0;
    bool is_dragging_ = false;
    
    // Panel hover state
    enum class HoveredPanel { None, Left, Center, Right };
    HoveredPanel hovered_panel_ = HoveredPanel::None;
    std::string hovered_device_id_;
    
    // Main view rectangle (updated during render, used for input)
    // This ensures render and input use exactly the same coordinates
    // Protected by view_rect_mutex_ for thread-safe access
    struct ViewRect {
        float x = 0, y = 0, w = 0, h = 0;
        bool valid = false;
    };
    ViewRect main_view_rect_;  // Set in renderCenterPanel, used in processMainViewClick
    mutable std::mutex view_rect_mutex_;  // Protects main_view_rect_
    
    // Callbacks
    TapCallback tap_callback_;
    SwipeCallback swipe_callback_;
    DeviceSelectCallback device_select_callback_;
    LearningDataCallback learning_data_callback_;
    StartMirroringCallback start_mirroring_callback_;
    
    // Window
    HWND hwnd_ = nullptr;
    int window_width_ = 1920;
    int window_height_ = 1080;
    std::atomic<bool> resizing_{false};  // Prevent render during resize
    bool frame_valid_{false};  // Set by vulkanBeginFrame, guards frame ops

    // Font scaling
    float base_font_size_ = 24.0f;   // Base font size at 1080p
    float current_font_scale_ = 1.0f;

    // ADB Device Manager
    ::gui::AdbDeviceManager* adb_manager_ = nullptr;

    // Screenshot popup
    bool show_screenshot_popup_ = false;
    std::vector<uint8_t> screenshot_data_;
    std::string screenshot_device_id_;
    VkDescriptorSet screenshot_vk_ds_ = VK_NULL_HANDLE;
    std::unique_ptr<mirage::vk::VulkanTexture> screenshot_vk_texture_;
    int screenshot_width_ = 0;
    int screenshot_height_ = 0;
};

// =============================================================================
// Status Color Helper
// =============================================================================
inline uint32_t GuiApplication::getStatusColor(DeviceStatus status) const {
    switch (status) {
        case DeviceStatus::Disconnected: return config_.color_disconnected;
        case DeviceStatus::Idle:         return config_.color_idle;
        case DeviceStatus::AndroidActive:return config_.color_android_active;
        case DeviceStatus::AIActive:     return config_.color_ai_active;
        case DeviceStatus::Stuck:        return config_.color_stuck;
        case DeviceStatus::Error:        return config_.color_error;
        default:                         return config_.color_idle;
    }
}

inline std::string GuiApplication::getStatusText(DeviceStatus status) const {
    switch (status) {
        case DeviceStatus::Disconnected: return u8"\u672a\u63a5\u7d9a";
        case DeviceStatus::Idle:         return u8"\u5f85\u6a5f\u4e2d";
        case DeviceStatus::AndroidActive:return u8"Android\u51e6\u7406\u4e2d";
        case DeviceStatus::AIActive:     return u8"AI\u5224\u5b9a\u4e2d";
        case DeviceStatus::Stuck:        return u8"\u8a70\u307e\u308a\u691c\u51fa";
        case DeviceStatus::Error:        return u8"\u30a8\u30e9\u30fc";
        default:                         return u8"\u4e0d\u660e";
    }
}

inline uint64_t GuiApplication::getCurrentTimeMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace mirage::gui
