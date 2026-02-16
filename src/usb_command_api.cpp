// =============================================================================
// MultiUsbCommandSender - Command API
// =============================================================================
// High-level command methods: send_tap, send_swipe, send_back, send_key, etc.
// These methods build protocol packets and queue them for transmission.
// =============================================================================

#include "multi_usb_command_sender.hpp"

#ifdef USE_LIBUSB

#include <cstring>
#include "mirage_log.hpp"
#include "mirage_protocol.hpp"

using namespace mirage::protocol;

namespace gui {

// =============================================================================
// Single-device command API
// =============================================================================

uint32_t MultiUsbCommandSender::send_ping(const std::string& usb_id) {
    return queue_command(usb_id, CMD_PING, nullptr, 0);
}

uint32_t MultiUsbCommandSender::send_tap(const std::string& usb_id, int x, int y, int screen_w, int screen_h) {
    uint8_t payload[20];

    payload[0] = x & 0xFF;
    payload[1] = (x >> 8) & 0xFF;
    payload[2] = (x >> 16) & 0xFF;
    payload[3] = (x >> 24) & 0xFF;

    payload[4] = y & 0xFF;
    payload[5] = (y >> 8) & 0xFF;
    payload[6] = (y >> 16) & 0xFF;
    payload[7] = (y >> 24) & 0xFF;

    payload[8] = screen_w & 0xFF;
    payload[9] = (screen_w >> 8) & 0xFF;
    payload[10] = (screen_w >> 16) & 0xFF;
    payload[11] = (screen_w >> 24) & 0xFF;

    payload[12] = screen_h & 0xFF;
    payload[13] = (screen_h >> 8) & 0xFF;
    payload[14] = (screen_h >> 16) & 0xFF;
    payload[15] = (screen_h >> 24) & 0xFF;

    payload[16] = 0;
    payload[17] = 0;
    payload[18] = 0;
    payload[19] = 0;

    uint32_t seq = queue_command(usb_id, CMD_TAP, payload, sizeof(payload));
    if (seq) {
        MLOG_INFO("multicmd", "Queued TAP(%d,%d) to %s seq=%u", x, y, usb_id.c_str(), seq);
    }
    return seq;
}

uint32_t MultiUsbCommandSender::send_swipe(const std::string& usb_id, int x1, int y1, int x2, int y2, int duration_ms) {
    uint8_t payload[20];

    payload[0] = x1 & 0xFF;
    payload[1] = (x1 >> 8) & 0xFF;
    payload[2] = (x1 >> 16) & 0xFF;
    payload[3] = (x1 >> 24) & 0xFF;

    payload[4] = y1 & 0xFF;
    payload[5] = (y1 >> 8) & 0xFF;
    payload[6] = (y1 >> 16) & 0xFF;
    payload[7] = (y1 >> 24) & 0xFF;

    payload[8] = x2 & 0xFF;
    payload[9] = (x2 >> 8) & 0xFF;
    payload[10] = (x2 >> 16) & 0xFF;
    payload[11] = (x2 >> 24) & 0xFF;

    payload[12] = y2 & 0xFF;
    payload[13] = (y2 >> 8) & 0xFF;
    payload[14] = (y2 >> 16) & 0xFF;
    payload[15] = (y2 >> 24) & 0xFF;

    payload[16] = duration_ms & 0xFF;
    payload[17] = (duration_ms >> 8) & 0xFF;
    payload[18] = (duration_ms >> 16) & 0xFF;
    payload[19] = (duration_ms >> 24) & 0xFF;

    uint32_t seq = queue_command(usb_id, CMD_SWIPE, payload, sizeof(payload));
    if (seq) {
        MLOG_INFO("multicmd", "Queued SWIPE(%d,%d)->(%d,%d) to %s seq=%u", x1, y1, x2, y2, usb_id.c_str(), seq);
    }
    return seq;
}

uint32_t MultiUsbCommandSender::send_back(const std::string& usb_id) {
    uint8_t payload[4] = {0, 0, 0, 0};
    uint32_t seq = queue_command(usb_id, CMD_BACK, payload, sizeof(payload));
    if (seq) {
        MLOG_INFO("multicmd", "Queued BACK to %s seq=%u", usb_id.c_str(), seq);
    }
    return seq;
}

uint32_t MultiUsbCommandSender::send_key(const std::string& usb_id, int keycode) {
    uint8_t payload[8];

    payload[0] = keycode & 0xFF;
    payload[1] = (keycode >> 8) & 0xFF;
    payload[2] = (keycode >> 16) & 0xFF;
    payload[3] = (keycode >> 24) & 0xFF;
    payload[4] = 0;
    payload[5] = 0;
    payload[6] = 0;
    payload[7] = 0;

    uint32_t seq = queue_command(usb_id, CMD_KEY, payload, sizeof(payload));
    if (seq) {
        MLOG_INFO("multicmd", "Queued KEY(%d) to %s seq=%u", keycode, usb_id.c_str(), seq);
    }
    return seq;
}

uint32_t MultiUsbCommandSender::send_click_id(const std::string& usb_id, const std::string& resource_id) {
    std::vector<uint8_t> payload(2 + resource_id.size());
    uint16_t len = (uint16_t)resource_id.size();
    payload[0] = len & 0xFF;
    payload[1] = (len >> 8) & 0xFF;
    memcpy(payload.data() + 2, resource_id.c_str(), resource_id.size());

    return queue_command(usb_id, CMD_CLICK_ID, payload.data(), payload.size());
}

uint32_t MultiUsbCommandSender::send_click_text(const std::string& usb_id, const std::string& text) {
    std::vector<uint8_t> payload(2 + text.size());
    uint16_t len = (uint16_t)text.size();
    payload[0] = len & 0xFF;
    payload[1] = (len >> 8) & 0xFF;
    memcpy(payload.data() + 2, text.c_str(), text.size());

    return queue_command(usb_id, CMD_CLICK_TEXT, payload.data(), payload.size());
}

// =============================================================================
// Broadcast command API (send to all devices)
// =============================================================================

int MultiUsbCommandSender::send_tap_all(int x, int y, int screen_w, int screen_h) {
    auto ids = get_device_ids();
    int count = 0;
    for (const auto& id : ids) {
        if (send_tap(id, x, y, screen_w, screen_h)) count++;
    }
    return count;
}

int MultiUsbCommandSender::send_swipe_all(int x1, int y1, int x2, int y2, int duration_ms) {
    auto ids = get_device_ids();
    int count = 0;
    for (const auto& id : ids) {
        if (send_swipe(id, x1, y1, x2, y2, duration_ms)) count++;
    }
    return count;
}

int MultiUsbCommandSender::send_back_all() {
    auto ids = get_device_ids();
    int count = 0;
    for (const auto& id : ids) {
        if (send_back(id)) count++;
    }
    return count;
}

int MultiUsbCommandSender::send_key_all(int keycode) {
    auto ids = get_device_ids();
    int count = 0;
    for (const auto& id : ids) {
        if (send_key(id, keycode)) count++;
    }
    return count;
}

// =============================================================================
// Video control commands
// =============================================================================

uint32_t MultiUsbCommandSender::send_video_fps(const std::string& usb_id, int fps) {
    uint8_t payload[4];
    payload[0] = fps & 0xFF;
    payload[1] = (fps >> 8) & 0xFF;
    payload[2] = 0;
    payload[3] = 0;
    return queue_command(usb_id, CMD_VIDEO_FPS, payload, sizeof(payload));
}

uint32_t MultiUsbCommandSender::send_video_route(const std::string& usb_id, uint8_t mode,
                                                   const std::string& host, int port) {
    std::vector<uint8_t> payload;
    payload.push_back(mode);  // 0=USB, 1=WiFi
    payload.push_back(port & 0xFF);
    payload.push_back((port >> 8) & 0xFF);
    for (char c : host) {
        payload.push_back(static_cast<uint8_t>(c));
    }
    payload.push_back(0);
    return queue_command(usb_id, CMD_VIDEO_ROUTE, payload.data(), payload.size());
}

uint32_t MultiUsbCommandSender::send_video_idr(const std::string& usb_id) {
    return queue_command(usb_id, CMD_VIDEO_IDR, nullptr, 0);
}

} // namespace gui

#endif // USE_LIBUSB
