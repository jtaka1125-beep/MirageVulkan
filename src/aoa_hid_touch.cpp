// =============================================================================
// MirageSystem - AOA HID Touch Controller Implementation
// =============================================================================
#include "aoa_hid_touch.hpp"
#include "mirage_log.hpp"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>

using namespace mirage::protocol;

namespace mirage {

// ─────────────────────────────────────────────────────────────────────────────
// Multitouch HID Report Descriptor (5 contacts, Touch Screen usage 0x04)
// ─────────────────────────────────────────────────────────────────────────────
// Per-contact: 1 bit tip_switch + 2 bit padding + 5 bit contact_id + 16 bit X + 16 bit Y = 40 bits = 5 bytes
// Report: 1 byte report_id + 5×5 bytes contacts + 1 byte contact_count = 27 bytes

// Macro for a single finger collection (saves repetition)
#define FINGER_COLLECTION \
    0x05, 0x0D,        /* USAGE_PAGE (Digitizers) */           \
    0x09, 0x22,        /* USAGE (Finger) */                    \
    0xA1, 0x02,        /* COLLECTION (Logical) */              \
    0x09, 0x42,        /*   USAGE (Tip Switch) */              \
    0x15, 0x00,        /*   LOGICAL_MINIMUM (0) */             \
    0x25, 0x01,        /*   LOGICAL_MAXIMUM (1) */             \
    0x75, 0x01,        /*   REPORT_SIZE (1) */                 \
    0x95, 0x01,        /*   REPORT_COUNT (1) */                \
    0x81, 0x02,        /*   INPUT (Data,Var,Abs) */            \
    0x95, 0x02,        /*   REPORT_COUNT (2) */                \
    0x81, 0x03,        /*   INPUT (Cnst,Var,Abs) [padding] */  \
    0x09, 0x51,        /*   USAGE (Contact Identifier) */      \
    0x25, 0x1F,        /*   LOGICAL_MAXIMUM (31) */            \
    0x75, 0x05,        /*   REPORT_SIZE (5) */                 \
    0x95, 0x01,        /*   REPORT_COUNT (1) */                \
    0x81, 0x02,        /*   INPUT (Data,Var,Abs) */            \
    0x05, 0x01,        /*   USAGE_PAGE (Generic Desktop) */    \
    0x09, 0x30,        /*   USAGE (X) */                       \
    0x15, 0x00,        /*   LOGICAL_MINIMUM (0) */             \
    0x26, 0xFF, 0x7F,  /*   LOGICAL_MAXIMUM (32767) */         \
    0x75, 0x10,        /*   REPORT_SIZE (16) */                \
    0x95, 0x01,        /*   REPORT_COUNT (1) */                \
    0x81, 0x02,        /*   INPUT (Data,Var,Abs) [X] */        \
    0x09, 0x31,        /*   USAGE (Y) */                       \
    0x81, 0x02,        /*   INPUT (Data,Var,Abs) [Y] */        \
    0xC0               /* END_COLLECTION */

static const uint8_t MULTITOUCH_HID_DESCRIPTOR[] = {
    0x05, 0x0D,        // USAGE_PAGE (Digitizers)
    0x09, 0x04,        // USAGE (Touch Screen) — NOT 0x05 (Touch Pad)!
    0xA1, 0x01,        // COLLECTION (Application)
    0x85, HID_TOUCH_REPORT_ID,  // REPORT_ID (1)

    // 5 finger collections
    FINGER_COLLECTION,  // Contact 0
    FINGER_COLLECTION,  // Contact 1
    FINGER_COLLECTION,  // Contact 2
    FINGER_COLLECTION,  // Contact 3
    FINGER_COLLECTION,  // Contact 4

    // Contact Count
    0x05, 0x0D,        // USAGE_PAGE (Digitizers)
    0x09, 0x54,        // USAGE (Contact Count)
    0x15, 0x00,        // LOGICAL_MINIMUM (0)
    0x25, 0x05,        // LOGICAL_MAXIMUM (5)
    0x75, 0x08,        // REPORT_SIZE (8)
    0x95, 0x01,        // REPORT_COUNT (1)
    0x81, 0x02,        // INPUT (Data,Var,Abs)

    // Contact Count Maximum (Feature report for hid-multitouch driver)
    0x85, 0x02,        // REPORT_ID (2)
    0x09, 0x55,        // USAGE (Contact Count Maximum)
    0x25, 0x05,        // LOGICAL_MAXIMUM (5)
    0x75, 0x08,        // REPORT_SIZE (8)
    0x95, 0x01,        // REPORT_COUNT (1)
    0xB1, 0x02,        // FEATURE (Data,Var,Abs)

    0xC0               // END_COLLECTION
};

#undef FINGER_COLLECTION

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

AoaHidTouch::~AoaHidTouch() {
#ifdef USE_LIBUSB
    if (registered_.load() && handle_) {
        unregister_device(handle_);
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// USB Control Transfers
// ─────────────────────────────────────────────────────────────────────────────

#ifdef USE_LIBUSB

bool AoaHidTouch::register_device(libusb_device_handle* handle) {
    if (!handle) return false;

    const uint16_t desc_size = sizeof(MULTITOUCH_HID_DESCRIPTOR);
    MLOG_INFO("aoa_hid", "Registering touch HID device (id=%d, desc_size=%d)",
              AOA_HID_TOUCH_ID, desc_size);

    // Step 1: REGISTER_HID — tell Android we're adding a HID device
    int ret = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_REGISTER_HID,
        AOA_HID_TOUCH_ID,      // wValue: device ID
        desc_size,              // wIndex: total descriptor size
        nullptr, 0, 1000);

    if (ret < 0) {
        MLOG_ERROR("aoa_hid", "REGISTER_HID failed: %s", libusb_error_name(ret));
        return false;
    }

    // Step 2: SET_HID_REPORT_DESC — send the descriptor (single chunk)
    ret = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_SET_HID_REPORT_DESC,
        AOA_HID_TOUCH_ID,      // wValue: device ID
        0,                      // wIndex: offset = 0
        (uint8_t*)MULTITOUCH_HID_DESCRIPTOR,
        desc_size, 1000);

    if (ret < 0) {
        MLOG_ERROR("aoa_hid", "SET_HID_REPORT_DESC failed: %s", libusb_error_name(ret));
        return false;
    }

    handle_ = handle;
    registered_.store(true);
    MLOG_INFO("aoa_hid", "Touch HID device registered successfully");
    return true;
}

bool AoaHidTouch::unregister_device(libusb_device_handle* handle) {
    if (!handle) return false;

    int ret = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_UNREGISTER_HID,
        AOA_HID_TOUCH_ID,
        0, nullptr, 0, 1000);

    registered_.store(false);
    handle_ = nullptr;

    if (ret < 0) {
        MLOG_ERROR("aoa_hid", "UNREGISTER_HID failed: %s", libusb_error_name(ret));
        return false;
    }

    MLOG_INFO("aoa_hid", "Touch HID device unregistered");
    return true;
}

void AoaHidTouch::set_handle(libusb_device_handle* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    handle_ = handle;
}

bool AoaHidTouch::send_report(const TouchReport& report) {
    if (!handle_ || !registered_.load()) return false;

    int ret = libusb_control_transfer(handle_,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_SEND_HID_EVENT,
        AOA_HID_TOUCH_ID,      // wValue: device ID
        0,                      // wIndex: 0
        (uint8_t*)&report,
        sizeof(report), 100);   // 100ms timeout for events

    if (ret < 0) {
        MLOG_ERROR("aoa_hid", "SEND_HID_EVENT failed: %s", libusb_error_name(ret));
        return false;
    }
    return true;
}

#endif // USE_LIBUSB

// ─────────────────────────────────────────────────────────────────────────────
// Coordinate Conversion
// ─────────────────────────────────────────────────────────────────────────────

uint16_t AoaHidTouch::pixel_to_hid_x(int px, int screen_w) {
    if (screen_w <= 1) return 0;
    if (px <= 0) return 0;
    if (px >= screen_w - 1) return HID_TOUCH_COORD_MAX;
    return (uint16_t)((int64_t)px * HID_TOUCH_COORD_MAX / (screen_w - 1));
}

uint16_t AoaHidTouch::pixel_to_hid_y(int py, int screen_h) {
    if (screen_h <= 1) return 0;
    if (py <= 0) return 0;
    if (py >= screen_h - 1) return HID_TOUCH_COORD_MAX;
    return (uint16_t)((int64_t)py * HID_TOUCH_COORD_MAX / (screen_h - 1));
}

// ─────────────────────────────────────────────────────────────────────────────
// Report Building
// ─────────────────────────────────────────────────────────────────────────────

uint8_t AoaHidTouch::pack_status(bool tip_switch, uint8_t contact_id) {
    // bit0 = tip_switch, bit1-2 = 0 (padding), bit3-7 = contact_id
    return (tip_switch ? 1 : 0) | ((contact_id & 0x1F) << 3);
}

TouchReport AoaHidTouch::build_report() const {
    TouchReport report;
    std::memset(&report, 0, sizeof(report));
    report.report_id = HID_TOUCH_REPORT_ID;

    uint8_t count = 0;
    for (int i = 0; i < HID_TOUCH_MAX_CONTACTS; i++) {
        const auto& c = contacts_[i];
        report.contacts[i].status = pack_status(c.active, c.contact_id);
        report.contacts[i].x = c.x;
        report.contacts[i].y = c.y;
        if (c.active) count++;
    }
    report.contact_count = count;
    return report;
}

// ─────────────────────────────────────────────────────────────────────────────
// Low-level Touch Operations
// ─────────────────────────────────────────────────────────────────────────────

bool AoaHidTouch::touch_down(uint8_t contact_id, uint16_t hid_x, uint16_t hid_y) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (contact_id >= HID_TOUCH_MAX_CONTACTS) return false;

    contacts_[contact_id].active = true;
    contacts_[contact_id].contact_id = contact_id;
    contacts_[contact_id].x = hid_x;
    contacts_[contact_id].y = hid_y;
    return true;
}

bool AoaHidTouch::touch_move(uint8_t contact_id, uint16_t hid_x, uint16_t hid_y) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (contact_id >= HID_TOUCH_MAX_CONTACTS) return false;
    if (!contacts_[contact_id].active) return false;

    contacts_[contact_id].x = hid_x;
    contacts_[contact_id].y = hid_y;
    return true;
}

bool AoaHidTouch::touch_up(uint8_t contact_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (contact_id >= HID_TOUCH_MAX_CONTACTS) return false;

    contacts_[contact_id].active = false;
    return true;
}

bool AoaHidTouch::touch_up_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < HID_TOUCH_MAX_CONTACTS; i++) {
        contacts_[i].active = false;
    }
    return true;
}

bool AoaHidTouch::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef USE_LIBUSB
    TouchReport report = build_report();
    return send_report(report);
#else
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// High-level Touch Operations
// ─────────────────────────────────────────────────────────────────────────────

bool AoaHidTouch::tap(int x, int y, int screen_w, int screen_h) {
    uint16_t hx = pixel_to_hid_x(x, screen_w);
    uint16_t hy = pixel_to_hid_y(y, screen_h);

    // Touch down
    touch_down(0, hx, hy);
    if (!flush()) return false;

    // Hold briefly (Android needs ≥10ms to register a tap)
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    // Touch up
    touch_up(0);
    return flush();
}

bool AoaHidTouch::swipe(int x1, int y1, int x2, int y2,
                         int screen_w, int screen_h, int duration_ms) {
    const int interval_ms = 12;  // ~83 Hz
    const int steps = std::max(1, duration_ms / interval_ms);

    uint16_t hx1 = pixel_to_hid_x(x1, screen_w);
    uint16_t hy1 = pixel_to_hid_y(y1, screen_h);
    uint16_t hx2 = pixel_to_hid_x(x2, screen_w);
    uint16_t hy2 = pixel_to_hid_y(y2, screen_h);

    // Touch down at start
    touch_down(0, hx1, hy1);
    if (!flush()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));

    // Interpolate move events
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        int ix = (int)hx1 + (int)(t * ((int)hx2 - (int)hx1));
        uint16_t cx = (uint16_t)std::clamp(ix, 0, (int)HID_TOUCH_COORD_MAX);
        int iy = (int)hy1 + (int)(t * ((int)hy2 - (int)hy1));
        uint16_t cy = (uint16_t)std::clamp(iy, 0, (int)HID_TOUCH_COORD_MAX);
        touch_move(0, cx, cy);
        if (!flush()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    // Touch up
    touch_up(0);
    return flush();
}

bool AoaHidTouch::long_press(int x, int y, int screen_w, int screen_h, int hold_ms) {
    uint16_t hx = pixel_to_hid_x(x, screen_w);
    uint16_t hy = pixel_to_hid_y(y, screen_h);

    touch_down(0, hx, hy);
    if (!flush()) return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));

    touch_up(0);
    return flush();
}

bool AoaHidTouch::pinch(int cx, int cy, int start_dist, int end_dist,
                         int screen_w, int screen_h, int duration_ms) {
    const int interval_ms = 12;
    const int steps = std::max(1, duration_ms / interval_ms);

    // Two fingers, horizontally symmetric around center
    auto put_fingers = [&](int dist) {
        int half = dist / 2;
        uint16_t hx0 = pixel_to_hid_x(std::max(0, cx - half), screen_w);
        uint16_t hx1 = pixel_to_hid_x(std::min(screen_w - 1, cx + half), screen_w);
        uint16_t hy = pixel_to_hid_y(cy, screen_h);
        contacts_[0] = { true, 0, hx0, hy };
        contacts_[1] = { true, 1, hx1, hy };
    };

    // Touch down both fingers
    {
        std::lock_guard<std::mutex> lock(mutex_);
        put_fingers(start_dist);
    }
    if (!flush()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));

    // Interpolate
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        int dist = (int)(start_dist + t * (end_dist - start_dist));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            put_fingers(dist);
        }
        if (!flush()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    // Release both
    touch_up(0);
    touch_up(1);
    return flush();
}

} // namespace mirage
