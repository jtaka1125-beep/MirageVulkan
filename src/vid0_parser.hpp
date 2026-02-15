// =============================================================================
// MirageSystem - VID0 Packet Parser
// =============================================================================
// Common parser for USB video packets with VID0 framing.
// VID0 format: [MAGIC(4)] [LENGTH(4)] [RTP_DATA(LENGTH)]
// MAGIC = 0x56494430 ("VID0" big endian)
// =============================================================================
#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>

namespace mirage::video {

static constexpr uint32_t VID0_MAGIC = 0x56494430;
static constexpr size_t VID0_HEADER_SIZE = 8;
static constexpr size_t RTP_MAX_LEN = 65535;
static constexpr size_t RTP_MIN_LEN = 12;
static constexpr size_t BUFFER_MAX = 128 * 1024;
static constexpr size_t BUFFER_TRIM = 32 * 1024;

struct ParseResult {
    std::vector<std::vector<uint8_t>> rtp_packets;
    int sync_errors = 0;
    bool buffer_overflow = false;
};

// Parse VID0 framed RTP packets from a byte buffer.
// Uses efficient index-based parsing with single erase.
// Modifies `buffer` in-place (consumed data is erased).
inline ParseResult parseVid0Packets(std::vector<uint8_t>& buffer) {
    ParseResult result;

    size_t pos = 0;
    while (pos + VID0_HEADER_SIZE <= buffer.size()) {
        uint32_t magic = (uint32_t(buffer[pos]) << 24) |
                        (uint32_t(buffer[pos + 1]) << 16) |
                        (uint32_t(buffer[pos + 2]) << 8) |
                        uint32_t(buffer[pos + 3]);

        if (magic != VID0_MAGIC) {
            result.sync_errors++;
            bool found = false;
            for (size_t i = pos + 1; i + 3 < buffer.size(); i++) {
                if (buffer[i] == 0x56 && buffer[i+1] == 0x49 &&
                    buffer[i+2] == 0x44 && buffer[i+3] == 0x30) {
                    pos = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                pos = (buffer.size() > 3) ? buffer.size() - 3 : buffer.size();
                break;
            }
            continue;
        }

        uint32_t pkt_len = (uint32_t(buffer[pos + 4]) << 24) |
                          (uint32_t(buffer[pos + 5]) << 16) |
                          (uint32_t(buffer[pos + 6]) << 8) |
                          uint32_t(buffer[pos + 7]);

        if (pkt_len > RTP_MAX_LEN || pkt_len < RTP_MIN_LEN) {
            pos++;
            continue;
        }

        if (pos + VID0_HEADER_SIZE + pkt_len > buffer.size()) {
            break;  // Need more data
        }

        result.rtp_packets.emplace_back(
            buffer.begin() + pos + VID0_HEADER_SIZE,
            buffer.begin() + pos + VID0_HEADER_SIZE + pkt_len);
        pos += VID0_HEADER_SIZE + pkt_len;
    }

    // Single erase at the end (O(n) once instead of multiple times)
    if (pos > 0) {
        buffer.erase(buffer.begin(), buffer.begin() + pos);
    }

    // Prevent unbounded buffer growth
    if (buffer.size() > BUFFER_MAX) {
        buffer.erase(buffer.begin(), buffer.end() - BUFFER_TRIM);
        result.buffer_overflow = true;
    }

    return result;
}

} // namespace mirage::video
