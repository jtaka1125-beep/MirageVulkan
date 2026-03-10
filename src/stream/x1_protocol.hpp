// =============================================================================
// X1 Stream Protocol — PC side constants (mirrors StreamProtocol.kt)
// =============================================================================
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace mirage::x1 {

// ── Ports ────────────────────────────────────────────────────────────────────
constexpr int PORT_CONTROL      = 50200;
constexpr int PORT_CANONICAL    = 50201;  // DEPRECATED (disabled)
constexpr int PORT_PRESENTATION = 50202;  // DEPRECATED (disabled)

// ── Frame header ─────────────────────────────────────────────────────────────
constexpr int    HEADER_SIZE  = 24;
constexpr uint32_t MAGIC      = 0x4D46524D;  // "MFRM"
constexpr uint8_t  VERSION    = 0x01;

// Lane
constexpr uint8_t LANE_CANONICAL    = 0x01;
constexpr uint8_t LANE_PRESENTATION = 0x02;

// Codec
constexpr uint8_t CODEC_JPEG = 0x01;
constexpr uint8_t CODEC_H264 = 0x02;

// Flags
constexpr uint8_t FLAG_KEYFRAME    = 0x01;
constexpr uint8_t FLAG_FIRST_FRAG  = 0x02;
constexpr uint8_t FLAG_LAST_FRAG   = 0x04;

// ── Canonical defaults ────────────────────────────────────────────────────────
constexpr int CANONICAL_W = 600;
constexpr int CANONICAL_H = 1000;
constexpr int CANONICAL_FPS = 30;

// ── MTU ───────────────────────────────────────────────────────────────────────
constexpr int MTU_PAYLOAD = 1400;

// ── TCP commands ──────────────────────────────────────────────────────────────
constexpr const char* CMD_HELLO    = "HELLO";
constexpr const char* CMD_CAPS     = "CAPS";
constexpr const char* CMD_START    = "START";
constexpr const char* CMD_OK       = "OK";
constexpr const char* CMD_IDR      = "IDR";
constexpr const char* CMD_PRES_ON  = "PRES_ON";
constexpr const char* CMD_PRES_OFF = "PRES_OFF";
constexpr const char* CMD_PING     = "PING";
constexpr const char* CMD_PONG     = "PONG";
constexpr const char* CMD_BYE      = "BYE";

// ── Parsed header ─────────────────────────────────────────────────────────────
struct FrameHeader {
    uint8_t  lane;
    uint8_t  codec;
    uint8_t  flags;
    uint32_t frame_id;
    uint16_t frag_idx;
    uint16_t frag_tot;
    uint64_t pts_us;

    bool is_keyframe()   const { return (flags & FLAG_KEYFRAME)   != 0; }
    bool is_first_frag() const { return (flags & FLAG_FIRST_FRAG) != 0; }
    bool is_last_frag()  const { return (flags & FLAG_LAST_FRAG)  != 0; }
    bool is_canonical()  const { return lane == LANE_CANONICAL; }
    bool is_single_frag() const { return frag_tot == 1; }
};

/**
 * Parse a 24-byte frame header from buf.
 * Returns nullopt if magic mismatch or buf too small.
 */
inline std::optional<FrameHeader> parse_header(const uint8_t* buf, size_t len) {
    if (len < static_cast<size_t>(HEADER_SIZE)) return std::nullopt;

    uint32_t magic = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                     ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
    if (magic != MAGIC) return std::nullopt;

    FrameHeader h{};
    h.lane    = buf[5];
    h.codec   = buf[6];
    h.flags   = buf[7];
    h.frame_id = ((uint32_t)buf[8]  << 24) | ((uint32_t)buf[9]  << 16) |
                 ((uint32_t)buf[10] <<  8) |  (uint32_t)buf[11];
    h.frag_idx = ((uint16_t)buf[12] << 8) | (uint16_t)buf[13];
    h.frag_tot = ((uint16_t)buf[14] << 8) | (uint16_t)buf[15];
    h.pts_us   = 0;
    for (int i = 0; i < 8; ++i)
        h.pts_us = (h.pts_us << 8) | (uint64_t)buf[16 + i];

    return h;
}

} // namespace mirage::x1
