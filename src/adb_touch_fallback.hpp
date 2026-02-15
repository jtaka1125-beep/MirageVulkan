// =============================================================================
// MirageSystem - ADB Touch Fallback
// =============================================================================
// WiFi/ADB-based touch input as fallback when AOA HID is unavailable.
// Uses `adb shell input` commands via system calls or TCP socket.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <mutex>
#include <functional>
#include <thread>

namespace mirage {

/**
 * ADB-based touch input fallback.
 *
 * Priority levels (lower = faster):
 *   1. ADB shell sendevent  (~50-110ms, multitouch capable)
 *   2. ADB shell input tap  (~150-300ms, single touch only)
 *
 * Thread-safe for concurrent calls.
 */
class AdbTouchFallback {
public:
    AdbTouchFallback() = default;
    ~AdbTouchFallback() = default;

    // Set target device serial for adb -s <serial>
    void set_device(const std::string& serial) {
        std::lock_guard<std::mutex> lock(mutex_);
        device_serial_ = serial;
    }

    // Single tap
    bool tap(int x, int y);

    // Swipe
    bool swipe(int x1, int y1, int x2, int y2, int duration_ms = 300);

    // Long press
    bool long_press(int x, int y, int hold_ms = 500);

    // Key event
    bool key(int keycode);

    // Back button
    bool back();

    // Get last operation latency (ms)
    int last_latency_ms() const { return last_latency_ms_.load(); }

    // Enable/disable (for testing or intentional bypass)
    void set_enabled(bool en) { enabled_.store(en); }
    bool is_enabled() const { return enabled_.load(); }

private:
    // Execute an adb command asynchronously (fire-and-forget, non-blocking)
    bool exec_adb_async(const std::string& args);

    // Execute an adb command synchronously (blocking)
    bool exec_adb_sync(const std::string& args);

    // Build "adb -s <serial>" prefix
    std::string adb_prefix() const;

    std::mutex mutex_;
    std::string device_serial_;
    std::atomic<bool> enabled_{true};
    std::atomic<int> last_latency_ms_{0};
};

} // namespace mirage
