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

uint32_t MultiUsbCommandSender::send_swipe(const std::string& usb_id, int x1, int y1, int x2, int y2, int duration_ms, int screen_w, int screen_h) {
    // ISSUE-18: payload 28 bytes = x1,y1,x2,y2,dur,screen_w,screen_h (7×int32)
    uint8_t payload[28];

    payload[0] = x1 & 0xFF;  payload[1] = (x1>>8)&0xFF;  payload[2] = (x1>>16)&0xFF;  payload[3] = (x1>>24)&0xFF;
    payload[4] = y1 & 0xFF;  payload[5] = (y1>>8)&0xFF;  payload[6] = (y1>>16)&0xFF;  payload[7] = (y1>>24)&0xFF;
    payload[8] = x2 & 0xFF;  payload[9] = (x2>>8)&0xFF;  payload[10]= (x2>>16)&0xFF;  payload[11]= (x2>>24)&0xFF;
    payload[12]= y2 & 0xFF;  payload[13]= (y2>>8)&0xFF;  payload[14]= (y2>>16)&0xFF;  payload[15]= (y2>>24)&0xFF;
    payload[16]= duration_ms&0xFF; payload[17]=(duration_ms>>8)&0xFF; payload[18]=(duration_ms>>16)&0xFF; payload[19]=(duration_ms>>24)&0xFF;
    payload[20]= screen_w&0xFF;   payload[21]=(screen_w>>8)&0xFF;   payload[22]=(screen_w>>16)&0xFF;   payload[23]=(screen_w>>24)&0xFF;
    payload[24]= screen_h&0xFF;   payload[25]=(screen_h>>8)&0xFF;   payload[26]=(screen_h>>16)&0xFF;   payload[27]=(screen_h>>24)&0xFF;

    uint32_t seq = queue_command(usb_id, CMD_SWIPE, payload, sizeof(payload));
    if (seq) {
        MLOG_INFO("multicmd", "Queued SWIPE(%d,%d)->(%d,%d) sw=%d sh=%d to %s seq=%u", x1,y1,x2,y2,screen_w,screen_h,usb_id.c_str(),seq);
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

uint32_t MultiUsbCommandSender::send_ui_tree_req(const std::string& usb_id) {
    uint32_t seq = queue_command(usb_id, CMD_UI_TREE_REQ, nullptr, 0);
    if (seq) {
        MLOG_INFO("multicmd", "Queued UI_TREE_REQ to %s seq=%u", usb_id.c_str(), seq);
    }
    return seq;
}

uint32_t MultiUsbCommandSender::send_click_id(const std::string& usb_id, const std::string& resource_id) {
    // FIX-C: payload_len in MIRA header already encodes length. No len prefix needed.
    // Android parser reads payloadLen bytes directly as UTF-8 (trimEnd null).
    uint32_t seq = queue_command(usb_id, CMD_CLICK_ID,
        reinterpret_cast<const uint8_t*>(resource_id.c_str()), resource_id.size());
    if (seq) {
        MLOG_INFO("multicmd", "Queued CLICK_ID(%.64s) to %s seq=%u",
            resource_id.c_str(), usb_id.c_str(), seq);
    }
    return seq;
}


uint32_t MultiUsbCommandSender::send_click_text(const std::string& usb_id, const std::string& text) {
    // FIX-C: payload_len in MIRA header already encodes length. No len prefix needed.
    // Android parser reads payloadLen bytes directly as UTF-8 (trimEnd null).
    uint32_t seq = queue_command(usb_id, CMD_CLICK_TEXT,
        reinterpret_cast<const uint8_t*>(text.c_str()), text.size());
    if (seq) {
        MLOG_INFO("multicmd", "Queued CLICK_TEXT(%.64s) to %s seq=%u",
            text.c_str(), usb_id.c_str(), seq);
    }
    return seq;
}


// =============================================================================
// FIX-B: Pinch and LongPress commands
// =============================================================================

uint32_t MultiUsbCommandSender::send_pinch(
        const std::string& usb_id,
        int cx, int cy, int start_dist, int end_dist,
        int duration_ms, int angle_deg100) {
    // Payload: cx(4)+cy(4)+start_dist(4)+end_dist(4)+dur_ms(4)+angle_deg100(4) = 24 bytes
    // angle_deg100: angle in degrees * 100 (e.g. 4500 = 45.00 deg)
    uint8_t payload[24];
    auto w32 = [](uint8_t* b, int32_t v) {
        b[0]=v&0xFF; b[1]=(v>>8)&0xFF; b[2]=(v>>16)&0xFF; b[3]=(v>>24)&0xFF;
    };
    w32(payload+0,  cx);
    w32(payload+4,  cy);
    w32(payload+8,  start_dist);
    w32(payload+12, end_dist);
    w32(payload+16, duration_ms);
    w32(payload+20, angle_deg100);
    uint32_t seq = queue_command(usb_id, CMD_PINCH, payload, sizeof(payload));
    if (seq) {
        MLOG_INFO("multicmd", "Queued PINCH center=(%d,%d) dist=%d->%d dur=%d to %s seq=%u",
            cx, cy, start_dist, end_dist, duration_ms, usb_id.c_str(), seq);
    }
    return seq;
}

uint32_t MultiUsbCommandSender::send_longpress(
        const std::string& usb_id, int x, int y, int duration_ms) {
    // Payload: x(4)+y(4)+dur_ms(4) = 12 bytes
    uint8_t payload[12];
    auto w32 = [](uint8_t* b, int32_t v) {
        b[0]=v&0xFF; b[1]=(v>>8)&0xFF; b[2]=(v>>16)&0xFF; b[3]=(v>>24)&0xFF;
    };
    w32(payload+0, x);
    w32(payload+4, y);
    w32(payload+8, duration_ms);
    uint32_t seq = queue_command(usb_id, CMD_LONGPRESS, payload, sizeof(payload));
    if (seq) {
        MLOG_INFO("multicmd", "Queued LONGPRESS(%d,%d) dur=%d to %s seq=%u",
            x, y, duration_ms, usb_id.c_str(), seq);
    }
    return seq;
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
    // Payload format matches Android Protocol.kt CMD_VIDEO_ROUTE parser:
    //   mode: int32 LE (0=USB, 1=WiFi)  — Android reads payloadData.int (4 bytes)
    //   port: int32 LE                  — Android reads payloadData.int (4 bytes)
    //   host: UTF-8 string + null terminator
    // Previously was: mode(1B) + port(2B LE) which caused mode=garbage on Android side.
    std::vector<uint8_t> payload;
    int32_t mode32 = static_cast<int32_t>(mode);
    int32_t port32 = port;
    payload.push_back(mode32 & 0xFF);
    payload.push_back((mode32 >> 8) & 0xFF);
    payload.push_back((mode32 >> 16) & 0xFF);
    payload.push_back((mode32 >> 24) & 0xFF);
    payload.push_back(port32 & 0xFF);
    payload.push_back((port32 >> 8) & 0xFF);
    payload.push_back((port32 >> 16) & 0xFF);
    payload.push_back((port32 >> 24) & 0xFF);
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
