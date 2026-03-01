#pragma once
#include "multi_usb_command_sender.hpp"
#include "aoa_hid_touch.hpp"
#include "adb_touch_fallback.hpp"
#include "rtt_tracker.hpp"
#include <memory>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <string>

namespace gui {

/**
 * Multi-Device USB Command Sender
 * Handles multiple Android devices via USB AOA.
 * 3-tier touch input: AOA HID > MIRA USB > ADB fallback.
 */
class HybridCommandSender {
public:
    using AckCallback = std::function<void(const std::string& device_id, uint32_t seq, uint8_t status)>;
    using VideoDataCallback = std::function<void(const std::string& device_id, const uint8_t* data, size_t len)>;

    // Touch input mode tracking
    enum class TouchMode { AOA_HID, MIRA_USB, ADB_FALLBACK };

    HybridCommandSender();
    ~HybridCommandSender();

    // Start/stop
    bool start();
    void stop();

    // Rescan for newly connected devices
    void rescan() { if (usb_sender_) usb_sender_->rescan(); }

    bool running() const { return running_; }

    // Device management
    int device_count() const;
    std::vector<std::string> get_device_ids() const;
    bool is_device_connected(const std::string& usb_id) const;
    std::string get_first_device_id() const;

    // Get device info
    bool get_device_info(const std::string& usb_id, MultiUsbCommandSender::DeviceInfo& out) const;

    // Set callback for ACK responses
    void set_ack_callback(AckCallback cb);

    // Set callback for video data from USB
    void set_video_callback(VideoDataCallback cb);

    // Send commands to specific device
    uint32_t send_ping(const std::string& device_id);
    uint32_t send_tap(const std::string& device_id, int x, int y, int screen_w = 0, int screen_h = 0);
    uint32_t send_swipe(const std::string& device_id, int x1, int y1, int x2, int y2, int duration_ms = 300, int screen_w = 0, int screen_h = 0);
    uint32_t send_back(const std::string& device_id);
    uint32_t send_key(const std::string& device_id, int keycode);
    uint32_t send_ui_tree_req(const std::string& device_id);
        uint32_t send_click_id(const std::string& device_id, const std::string& resource_id);
    uint32_t send_click_text(const std::string& device_id, const std::string& text);

    // Long press and pinch (only available via HID or ADB)
    bool send_long_press(const std::string& device_id, int x, int y, int screen_w, int screen_h, int hold_ms = 500);
    bool send_pinch(const std::string& device_id, int cx, int cy, int start_dist, int end_dist, int screen_w, int screen_h, int duration_ms = 400);

    // Send commands to ALL connected devices
    int send_tap_all(int x, int y, int screen_w = 0, int screen_h = 0);
    int send_swipe_all(int x1, int y1, int x2, int y2, int duration_ms = 300, int screen_w = 0, int screen_h = 0);
    int send_back_all();
    int send_key_all(int keycode);

    // Legacy API (sends to first device) - for backward compatibility
    uint32_t send_ping();
    uint32_t send_tap(int x, int y, int screen_w = 0, int screen_h = 0);
    uint32_t send_swipe(int x1, int y1, int x2, int y2, int duration_ms = 300, int screen_w = 0, int screen_h = 0);  // ISSUE-18
    uint32_t send_back();
    uint32_t send_key(int keycode);
    uint32_t send_click_id(const std::string& resource_id);
    uint32_t send_click_text(const std::string& text);

    // Stats
    bool usb_connected() const;
    uint64_t usb_commands_sent() const;

    // Deprecated - kept for compatibility
    bool wifi_connected() const { return false; }
    uint64_t wifi_commands_sent() const { return 0; }

    // Get AOA HID touch controller (first device, backward compat)
    std::shared_ptr<mirage::AoaHidTouch> get_hid_touch() {
        std::lock_guard<std::mutex> lock(hid_mutex_);
        if (hid_touches_.empty()) return nullptr;
        return hid_touches_.begin()->second;
    }

    // Get HID touch for a specific device (returns shared_ptr to prevent UAF)
    std::shared_ptr<mirage::AoaHidTouch> get_hid_for_device(const std::string& device_id);

    // Get current touch mode
    TouchMode get_touch_mode() const { return current_touch_mode_.load(); }
    std::string get_touch_mode_str() const;

    // ADB fallback
    mirage::AdbTouchFallback* get_adb_fallback() { return adb_fallback_.get(); }

    // Video control commands
    uint32_t send_video_fps(const std::string& device_id, int fps) {
        return usb_sender_ ? usb_sender_->send_video_fps(device_id, fps) : 0;
    }
    uint32_t send_video_route(const std::string& device_id, uint8_t mode, const std::string& host, int port) {
        return usb_sender_ ? usb_sender_->send_video_route(device_id, mode, host, port) : 0;
    }
    uint32_t send_video_idr(const std::string& device_id) {
        return usb_sender_ ? usb_sender_->send_video_idr(device_id) : 0;
    }
    uint64_t total_bytes_received() const {
        return usb_sender_ ? usb_sender_->total_bytes_received() : 0;
    }

private:
    bool running_ = false;
    std::unique_ptr<MultiUsbCommandSender> usb_sender_;
    AckCallback ack_callback_;
    VideoDataCallback video_callback_;

    // AOA HID touch per device (primary path)
    std::map<std::string, std::shared_ptr<mirage::AoaHidTouch>> hid_touches_;
    mutable std::mutex hid_mutex_;  // Protects hid_touches_ from concurrent access

    // ADB fallback (last resort)
    std::unique_ptr<mirage::AdbTouchFallback> adb_fallback_;

    // Screen dimensions cache per device (for HID coordinate conversion)
    struct ScreenInfo {
        int width = 0;
        int height = 0;
    };
    std::map<std::string, ScreenInfo> screen_info_;

    // Touch input mode tracking
    std::atomic<TouchMode> current_touch_mode_{TouchMode::MIRA_USB};

    // RTT計測
    mirage::RttTracker rtt_tracker_;

    // Internal: try AOA HID tap/swipe for a specific device, return true if succeeded
    bool try_hid_tap(const std::string& device_id, int x, int y, int screen_w, int screen_h);
    bool try_hid_swipe(const std::string& device_id, int x1, int y1, int x2, int y2, int screen_w, int screen_h, int duration_ms);
};

} // namespace gui
