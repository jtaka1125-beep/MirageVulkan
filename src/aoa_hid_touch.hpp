// =============================================================================
// MirageSystem - AOA HID Touch Controller
// =============================================================================
// Sends multitouch HID events to Android via AOA v2 protocol.
// Requires libusb and an AOA-mode device handle.
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>

#ifdef USE_LIBUSB
#include <libusb-1.0/libusb.h>
#endif

#include "mirage_protocol.hpp"

namespace mirage {

// Per-contact touch state
struct TouchContact {
    bool     active;      // true = finger touching
    uint8_t  contact_id;  // 0-31
    uint16_t x;           // 0 - HID_TOUCH_COORD_MAX (32767)
    uint16_t y;           // 0 - HID_TOUCH_COORD_MAX (32767)
};

// 27-byte HID touch report (packed, little-endian)
#pragma pack(push, 1)
struct TouchReport {
    uint8_t report_id;    // 0x01
    struct ContactSlot {
        uint8_t  status;  // bit0=tip_switch, bit1-2=padding, bit3-7=contact_id
        uint16_t x;       // little-endian
        uint16_t y;       // little-endian
    } contacts[protocol::HID_TOUCH_MAX_CONTACTS];
    uint8_t contact_count;
};
#pragma pack(pop)
static_assert(sizeof(TouchReport) == protocol::HID_TOUCH_REPORT_SIZE, "TouchReport must be 27 bytes");

/**
 * AOA HID Touch Controller
 *
 * Usage:
 *   AoaHidTouch touch;
 *   touch.register_device(handle);   // before AOA_START_ACCESSORY
 *   // ... device re-enumerates ...
 *   touch.set_handle(new_handle);    // after re-open
 *   touch.tap(500, 800, 1080, 1920); // tap at pixel (500, 800) on 1080x1920 screen
 *   touch.swipe(100, 500, 900, 500, 300); // swipe
 */
class AoaHidTouch {
public:
    AoaHidTouch() = default;
    ~AoaHidTouch();

    // Non-copyable
    AoaHidTouch(const AoaHidTouch&) = delete;
    AoaHidTouch& operator=(const AoaHidTouch&) = delete;

#ifdef USE_LIBUSB
    // Register HID touch device (call BEFORE AOA_START_ACCESSORY)
    bool register_device(libusb_device_handle* handle);

    // Unregister HID touch device
    bool unregister_device(libusb_device_handle* handle);

    // Set new handle after device re-enumeration
    void set_handle(libusb_device_handle* handle);

    // Get current handle
    libusb_device_handle* get_handle() const { return handle_; }
#endif

    bool is_registered() const { return registered_.load(); }

    // Mark as unregistered without sending USB command (for disconnected devices)
    void mark_unregistered() {
        registered_.store(false);
#ifdef USE_LIBUSB
        handle_ = nullptr;
#endif
    }

    // ── High-level operations (pixel coordinates) ──

    // Single tap at pixel (x, y) on screen of (screen_w x screen_h)
    bool tap(int x, int y, int screen_w, int screen_h);

    // Swipe from (x1,y1) to (x2,y2) over duration_ms milliseconds
    bool swipe(int x1, int y1, int x2, int y2, int screen_w, int screen_h, int duration_ms = 300);

    // Long press at (x, y) for hold_ms milliseconds
    bool long_press(int x, int y, int screen_w, int screen_h, int hold_ms = 500);

    // Two-finger pinch (zoom in/out)
    bool pinch(int cx, int cy, int start_dist, int end_dist, int screen_w, int screen_h, int duration_ms = 400);

    // ── Low-level operations (HID coordinates 0-32767) ──

    // Touch down (begin contact)
    bool touch_down(uint8_t contact_id, uint16_t hid_x, uint16_t hid_y);

    // Touch move (update position)
    bool touch_move(uint8_t contact_id, uint16_t hid_x, uint16_t hid_y);

    // Touch up (release contact)
    bool touch_up(uint8_t contact_id);

    // Release all contacts
    bool touch_up_all();

    // Flush current contact state as HID report
    bool flush();

    // ── Coordinate conversion ──
    static uint16_t pixel_to_hid_x(int px, int screen_w);
    static uint16_t pixel_to_hid_y(int py, int screen_h);
    static uint8_t pack_status(bool tip_switch, uint8_t contact_id);

private:
#ifdef USE_LIBUSB
    bool send_report(const TouchReport& report);
    libusb_device_handle* handle_ = nullptr;
#endif

    std::atomic<bool> registered_{false};
    std::mutex mutex_;

    // Current contact states
    TouchContact contacts_[protocol::HID_TOUCH_MAX_CONTACTS] = {};

    // Build report from current contact states
    TouchReport build_report() const;

};

} // namespace mirage
