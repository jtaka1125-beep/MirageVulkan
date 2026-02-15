// =============================================================================
// MirageSystem - Protocol Constants & Utilities
// =============================================================================
// Shared protocol definitions for USB AOA and WiFi communication.
// Matches Android Protocol.kt implementation.
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace mirage::protocol {

// Protocol identification (matches UsbCommandSender and Android Protocol.kt)
static constexpr uint32_t PROTOCOL_MAGIC = 0x4D495241;  // "MIRA" little-endian
static constexpr uint8_t PROTOCOL_VERSION = 1;

// Packet header (14 bytes):
//   magic:   4 bytes (0x4D495241 = "MIRA" LE)
//   version: 1 byte  (1)
//   cmd:     1 byte
//   seq:     4 bytes
//   len:     4 bytes (payload length)
static constexpr size_t HEADER_SIZE = 14;

// Command types (PC -> Android)
static constexpr uint8_t CMD_PING       = 0x00;
static constexpr uint8_t CMD_TAP        = 0x01;
static constexpr uint8_t CMD_BACK       = 0x02;
static constexpr uint8_t CMD_KEY        = 0x03;
static constexpr uint8_t CMD_CONFIG     = 0x04;
static constexpr uint8_t CMD_CLICK_ID   = 0x05;
static constexpr uint8_t CMD_CLICK_TEXT = 0x06;
static constexpr uint8_t CMD_SWIPE      = 0x07;

// Video control commands (PC -> Android)
static constexpr uint8_t CMD_VIDEO_FPS   = 0x24;
static constexpr uint8_t CMD_VIDEO_ROUTE = 0x25;
static constexpr uint8_t CMD_VIDEO_IDR   = 0x26;
static constexpr uint8_t CMD_DEVICE_INFO = 0x27;

// Special frames
static constexpr uint8_t CMD_AUDIO_FRAME = 0x10;  // Audio: Android -> PC

// Response types (Android -> PC)
static constexpr uint8_t CMD_ACK   = 0x80;

// Status codes
static constexpr uint8_t STATUS_OK                  = 0;
static constexpr uint8_t STATUS_ERR_UNKNOWN_CMD     = 1;
static constexpr uint8_t STATUS_ERR_INVALID_PAYLOAD = 2;
static constexpr uint8_t STATUS_ERR_BUSY            = 3;
static constexpr uint8_t STATUS_ERR_NOT_FOUND       = 4;

// Packet limits
static constexpr size_t MAX_PAYLOAD = 4096;

// AOA USB constants
static constexpr uint16_t AOA_VID = 0x18D1;
static constexpr uint16_t AOA_PID_ACCESSORY           = 0x2D01;
static constexpr uint16_t AOA_PID_ACCESSORY_ADB       = 0x2D00;
static constexpr uint16_t AOA_PID_AUDIO                = 0x2D02;
static constexpr uint16_t AOA_PID_AUDIO_ADB            = 0x2D03;
static constexpr uint16_t AOA_PID_ACCESSORY_AUDIO      = 0x2D04;
static constexpr uint16_t AOA_PID_ACCESSORY_AUDIO_ADB  = 0x2D05;

// AOA protocol requests
static constexpr uint8_t AOA_GET_PROTOCOL   = 51;
static constexpr uint8_t AOA_SEND_STRING    = 52;
static constexpr uint8_t AOA_START_ACCESSORY = 53;

// AOA string indices
static constexpr uint16_t AOA_STRING_MANUFACTURER = 0;
static constexpr uint16_t AOA_STRING_MODEL        = 1;
static constexpr uint16_t AOA_STRING_DESCRIPTION  = 2;
static constexpr uint16_t AOA_STRING_VERSION      = 3;
static constexpr uint16_t AOA_STRING_URI          = 4;
static constexpr uint16_t AOA_STRING_SERIAL       = 5;

// AOA HID requests (AOA v2)
static constexpr uint8_t AOA_REGISTER_HID        = 54;  // 0x36
static constexpr uint8_t AOA_UNREGISTER_HID       = 55;  // 0x37
static constexpr uint8_t AOA_SET_HID_REPORT_DESC  = 56;  // 0x38
static constexpr uint8_t AOA_SEND_HID_EVENT       = 57;  // 0x39

// AOA HID device IDs
static constexpr uint16_t AOA_HID_TOUCH_ID = 1;
static constexpr uint16_t AOA_HID_KEYBOARD_ID = 2;

// HID touch constants
static constexpr uint16_t HID_TOUCH_MAX_CONTACTS = 5;
static constexpr uint16_t HID_TOUCH_COORD_MAX = 32767;
static constexpr uint8_t  HID_TOUCH_REPORT_ID = 0x01;
static constexpr size_t   HID_TOUCH_REPORT_SIZE = 27;  // 1 + 5*5 + 1

// =============================================================================
// Packet header structure (for zero-copy parsing)
// =============================================================================
struct PacketHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  cmd;
    uint32_t seq;
    uint32_t payload_len;
};

// =============================================================================
// Header builder — writes 14-byte header into buffer
// Returns pointer past the header (buf + HEADER_SIZE)
// =============================================================================
inline uint8_t* build_header(uint8_t* buf, uint8_t cmd, uint32_t seq, uint32_t payload_len) {
    uint32_t magic = PROTOCOL_MAGIC;
    memcpy(buf + 0, &magic, 4);
    buf[4] = PROTOCOL_VERSION;
    buf[5] = cmd;
    memcpy(buf + 6, &seq, 4);
    memcpy(buf + 10, &payload_len, 4);
    return buf + HEADER_SIZE;
}

// =============================================================================
// Header parser — reads 14-byte header from buffer
// Returns true if valid MIRA header
// =============================================================================
inline bool parse_header(const uint8_t* buf, size_t len, PacketHeader& out) {
    if (len < HEADER_SIZE) return false;
    memcpy(&out.magic, buf + 0, 4);
    if (out.magic != PROTOCOL_MAGIC) return false;
    out.version = buf[4];
    out.cmd = buf[5];
    memcpy(&out.seq, buf + 6, 4);
    memcpy(&out.payload_len, buf + 10, 4);
    return out.payload_len <= MAX_PAYLOAD;
}

// =============================================================================
// Full packet builder — header + payload
// =============================================================================
inline std::vector<uint8_t> build_packet(uint8_t cmd, uint32_t seq,
                                          const uint8_t* payload = nullptr,
                                          uint32_t payload_len = 0) {
    std::vector<uint8_t> pkt(HEADER_SIZE + payload_len);
    build_header(pkt.data(), cmd, seq, payload_len);
    if (payload && payload_len > 0) {
        memcpy(pkt.data() + HEADER_SIZE, payload, payload_len);
    }
    return pkt;
}

// =============================================================================
// AOA PID helpers
// =============================================================================
inline bool is_aoa_pid(uint16_t pid) {
    return pid >= AOA_PID_ACCESSORY_ADB && pid <= AOA_PID_ACCESSORY_AUDIO_ADB;
}

inline bool aoa_pid_has_adb(uint16_t pid) {
    return pid == AOA_PID_ACCESSORY_ADB || pid == AOA_PID_AUDIO_ADB ||
           pid == AOA_PID_ACCESSORY_AUDIO_ADB;
}

inline bool aoa_pid_has_audio(uint16_t pid) {
    return pid == AOA_PID_AUDIO || pid == AOA_PID_AUDIO_ADB ||
           pid == AOA_PID_ACCESSORY_AUDIO || pid == AOA_PID_ACCESSORY_AUDIO_ADB;
}

// =============================================================================
// Command name for logging
// =============================================================================
inline const char* cmd_name(uint8_t cmd) {
    switch (cmd) {
        case CMD_PING:        return "PING";
        case CMD_TAP:         return "TAP";
        case CMD_BACK:        return "BACK";
        case CMD_KEY:         return "KEY";
        case CMD_CONFIG:      return "CONFIG";
        case CMD_CLICK_ID:    return "CLICK_ID";
        case CMD_CLICK_TEXT:  return "CLICK_TEXT";
        case CMD_SWIPE:       return "SWIPE";
        case CMD_VIDEO_FPS:   return "VIDEO_FPS";
        case CMD_VIDEO_ROUTE: return "VIDEO_ROUTE";
        case CMD_VIDEO_IDR:   return "VIDEO_IDR";
        case CMD_DEVICE_INFO: return "DEVICE_INFO";
        case CMD_AUDIO_FRAME: return "AUDIO_FRAME";
        case CMD_ACK:         return "ACK";
        default:              return "UNKNOWN";
    }
}

} // namespace mirage::protocol
