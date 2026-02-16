// =============================================================================
// Unit tests for USB command packet building and parsing
// =============================================================================
// Tests the command packet construction logic used by UsbCommandSender
// and MultiUsbCommandSender without requiring actual USB hardware.
// =============================================================================
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

// Include protocol definitions
#include "mirage_protocol.hpp"

using namespace mirage::protocol;

namespace {

// Helper: build TAP command payload (matches usb_command_api.cpp)
std::vector<uint8_t> buildTapPayload(int x, int y, int screen_w, int screen_h) {
    std::vector<uint8_t> payload(20);

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

    return payload;
}

// Helper: build SWIPE command payload
std::vector<uint8_t> buildSwipePayload(int x1, int y1, int x2, int y2, int duration_ms) {
    std::vector<uint8_t> payload(20);

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

    return payload;
}

// Helper: build KEY command payload
std::vector<uint8_t> buildKeyPayload(int keycode) {
    std::vector<uint8_t> payload(8);

    payload[0] = keycode & 0xFF;
    payload[1] = (keycode >> 8) & 0xFF;
    payload[2] = (keycode >> 16) & 0xFF;
    payload[3] = (keycode >> 24) & 0xFF;
    payload[4] = 0;
    payload[5] = 0;
    payload[6] = 0;
    payload[7] = 0;

    return payload;
}

// Helper: parse little-endian 32-bit value
inline uint32_t readLE32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

} // anonymous namespace

// ===========================================================================
// TAP command tests
// ===========================================================================
TEST(UsbCommand, TapPayloadStructure) {
    auto payload = buildTapPayload(100, 200, 1080, 1920);

    EXPECT_EQ(payload.size(), 20u);
    EXPECT_EQ(readLE32(&payload[0]), 100u);   // x
    EXPECT_EQ(readLE32(&payload[4]), 200u);   // y
    EXPECT_EQ(readLE32(&payload[8]), 1080u);  // screen_w
    EXPECT_EQ(readLE32(&payload[12]), 1920u); // screen_h
    EXPECT_EQ(readLE32(&payload[16]), 0u);    // reserved
}

TEST(UsbCommand, TapFullPacket) {
    auto payload = buildTapPayload(500, 600, 1080, 2400);
    auto packet = build_packet(CMD_TAP, 42, payload.data(), static_cast<uint32_t>(payload.size()));

    EXPECT_EQ(packet.size(), HEADER_SIZE + 20);

    PacketHeader hdr;
    EXPECT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.magic, PROTOCOL_MAGIC);
    EXPECT_EQ(hdr.version, PROTOCOL_VERSION);
    EXPECT_EQ(hdr.cmd, CMD_TAP);
    EXPECT_EQ(hdr.seq, 42u);
    EXPECT_EQ(hdr.payload_len, 20u);
}

TEST(UsbCommand, TapLargeCoordinates) {
    // Test large coordinates that use all 4 bytes
    auto payload = buildTapPayload(32767, 65535, 4096, 8192);

    EXPECT_EQ(readLE32(&payload[0]), 32767u);
    EXPECT_EQ(readLE32(&payload[4]), 65535u);
    EXPECT_EQ(readLE32(&payload[8]), 4096u);
    EXPECT_EQ(readLE32(&payload[12]), 8192u);
}

TEST(UsbCommand, TapZeroCoordinates) {
    auto payload = buildTapPayload(0, 0, 0, 0);

    EXPECT_EQ(readLE32(&payload[0]), 0u);
    EXPECT_EQ(readLE32(&payload[4]), 0u);
    EXPECT_EQ(readLE32(&payload[8]), 0u);
    EXPECT_EQ(readLE32(&payload[12]), 0u);
}

// ===========================================================================
// SWIPE command tests
// ===========================================================================
TEST(UsbCommand, SwipePayloadStructure) {
    auto payload = buildSwipePayload(100, 200, 300, 400, 500);

    EXPECT_EQ(payload.size(), 20u);
    EXPECT_EQ(readLE32(&payload[0]), 100u);   // x1
    EXPECT_EQ(readLE32(&payload[4]), 200u);   // y1
    EXPECT_EQ(readLE32(&payload[8]), 300u);   // x2
    EXPECT_EQ(readLE32(&payload[12]), 400u);  // y2
    EXPECT_EQ(readLE32(&payload[16]), 500u);  // duration_ms
}

TEST(UsbCommand, SwipeFullPacket) {
    auto payload = buildSwipePayload(0, 500, 1000, 500, 300);
    auto packet = build_packet(CMD_SWIPE, 123, payload.data(), static_cast<uint32_t>(payload.size()));

    PacketHeader hdr;
    EXPECT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_SWIPE);
    EXPECT_EQ(hdr.seq, 123u);
    EXPECT_EQ(hdr.payload_len, 20u);
}

TEST(UsbCommand, SwipeLongDuration) {
    // Test with long duration (e.g., 10 seconds = 10000ms)
    auto payload = buildSwipePayload(0, 0, 1000, 1000, 10000);
    EXPECT_EQ(readLE32(&payload[16]), 10000u);
}

// ===========================================================================
// KEY command tests
// ===========================================================================
TEST(UsbCommand, KeyPayloadStructure) {
    // Android KEYCODE_BACK = 4
    auto payload = buildKeyPayload(4);

    EXPECT_EQ(payload.size(), 8u);
    EXPECT_EQ(readLE32(&payload[0]), 4u);
    EXPECT_EQ(readLE32(&payload[4]), 0u);
}

TEST(UsbCommand, KeyFullPacket) {
    auto payload = buildKeyPayload(66);  // KEYCODE_ENTER
    auto packet = build_packet(CMD_KEY, 999, payload.data(), static_cast<uint32_t>(payload.size()));

    PacketHeader hdr;
    EXPECT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_KEY);
    EXPECT_EQ(hdr.payload_len, 8u);
}

TEST(UsbCommand, KeyCommonKeycodes) {
    // Test common Android keycodes
    struct TestCase {
        int keycode;
        const char* name;
    };

    TestCase cases[] = {
        {3, "HOME"},
        {4, "BACK"},
        {24, "VOLUME_UP"},
        {25, "VOLUME_DOWN"},
        {26, "POWER"},
        {66, "ENTER"},
        {82, "MENU"},
        {187, "APP_SWITCH"},
    };

    for (const auto& tc : cases) {
        auto payload = buildKeyPayload(tc.keycode);
        EXPECT_EQ(readLE32(&payload[0]), static_cast<uint32_t>(tc.keycode))
            << "Failed for keycode: " << tc.name;
    }
}

// ===========================================================================
// PING and BACK command tests (no payload)
// ===========================================================================
TEST(UsbCommand, PingPacket) {
    auto packet = build_packet(CMD_PING, 1);

    EXPECT_EQ(packet.size(), HEADER_SIZE);

    PacketHeader hdr;
    EXPECT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_PING);
    EXPECT_EQ(hdr.payload_len, 0u);
}

TEST(UsbCommand, BackPacket) {
    uint8_t payload[4] = {0, 0, 0, 0};
    auto packet = build_packet(CMD_BACK, 55, payload, sizeof(payload));

    PacketHeader hdr;
    EXPECT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_BACK);
    EXPECT_EQ(hdr.payload_len, 4u);
}

// ===========================================================================
// Video control command tests
// ===========================================================================
TEST(UsbCommand, VideoFpsPayload) {
    uint8_t payload[4];
    int fps = 30;
    payload[0] = fps & 0xFF;
    payload[1] = (fps >> 8) & 0xFF;
    payload[2] = 0;
    payload[3] = 0;

    auto packet = build_packet(CMD_VIDEO_FPS, 100, payload, sizeof(payload));

    PacketHeader hdr;
    EXPECT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_VIDEO_FPS);
    EXPECT_EQ(readLE32(packet.data() + HEADER_SIZE), 30u);
}

TEST(UsbCommand, VideoIdrPacket) {
    auto packet = build_packet(CMD_VIDEO_IDR, 200);

    PacketHeader hdr;
    EXPECT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_VIDEO_IDR);
    EXPECT_EQ(hdr.payload_len, 0u);
}

// ===========================================================================
// ACK response parsing tests
// ===========================================================================
TEST(UsbCommand, ParseAckResponse) {
    // Build ACK response with status
    uint8_t status_payload[5] = {0, 0, 0, 0, STATUS_OK};
    auto packet = build_packet(CMD_ACK, 42, status_payload, sizeof(status_payload));

    PacketHeader hdr;
    EXPECT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_ACK);
    EXPECT_EQ(hdr.seq, 42u);

    // Extract status from payload
    uint8_t status = packet[HEADER_SIZE + 4];
    EXPECT_EQ(status, STATUS_OK);
}

TEST(UsbCommand, ParseAckErrorStatus) {
    uint8_t status_payload[5] = {0, 0, 0, 0, STATUS_ERR_NOT_FOUND};
    auto packet = build_packet(CMD_ACK, 100, status_payload, sizeof(status_payload));

    uint8_t status = packet[HEADER_SIZE + 4];
    EXPECT_EQ(status, STATUS_ERR_NOT_FOUND);
}

// ===========================================================================
// Sequence number tests
// ===========================================================================
TEST(UsbCommand, SequenceNumberInPacket) {
    auto packet1 = build_packet(CMD_PING, 1);
    auto packet2 = build_packet(CMD_PING, 65535);
    auto packet3 = build_packet(CMD_PING, 0xFFFFFFFF);

    PacketHeader hdr1, hdr2, hdr3;
    EXPECT_TRUE(parse_header(packet1.data(), packet1.size(), hdr1));
    EXPECT_TRUE(parse_header(packet2.data(), packet2.size(), hdr2));
    EXPECT_TRUE(parse_header(packet3.data(), packet3.size(), hdr3));

    EXPECT_EQ(hdr1.seq, 1u);
    EXPECT_EQ(hdr2.seq, 65535u);
    EXPECT_EQ(hdr3.seq, 0xFFFFFFFFu);
}

// ===========================================================================
// Payload size limit tests
// ===========================================================================
TEST(UsbCommand, MaxPayloadSize) {
    // MAX_PAYLOAD is 4096
    EXPECT_EQ(MAX_PAYLOAD, 4096u);

    // Valid payload at max size
    std::vector<uint8_t> payload(MAX_PAYLOAD, 0x42);
    auto packet = build_packet(CMD_CONFIG, 1, payload.data(), static_cast<uint32_t>(payload.size()));

    PacketHeader hdr;
    EXPECT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.payload_len, MAX_PAYLOAD);
}

TEST(UsbCommand, RejectOversizedPayload) {
    // Create a header with payload_len > MAX_PAYLOAD
    std::vector<uint8_t> fake_packet(HEADER_SIZE);
    uint32_t magic = PROTOCOL_MAGIC;
    memcpy(&fake_packet[0], &magic, 4);
    fake_packet[4] = PROTOCOL_VERSION;
    fake_packet[5] = CMD_CONFIG;
    uint32_t seq = 1;
    memcpy(&fake_packet[6], &seq, 4);
    uint32_t bad_len = MAX_PAYLOAD + 1;  // Too large
    memcpy(&fake_packet[10], &bad_len, 4);

    PacketHeader hdr;
    EXPECT_FALSE(parse_header(fake_packet.data(), fake_packet.size(), hdr));
}

// ===========================================================================
// Command name utility tests
// ===========================================================================
TEST(UsbCommand, CommandNames) {
    EXPECT_STREQ(cmd_name(CMD_PING), "PING");
    EXPECT_STREQ(cmd_name(CMD_TAP), "TAP");
    EXPECT_STREQ(cmd_name(CMD_BACK), "BACK");
    EXPECT_STREQ(cmd_name(CMD_KEY), "KEY");
    EXPECT_STREQ(cmd_name(CMD_SWIPE), "SWIPE");
    EXPECT_STREQ(cmd_name(CMD_VIDEO_FPS), "VIDEO_FPS");
    EXPECT_STREQ(cmd_name(CMD_VIDEO_IDR), "VIDEO_IDR");
    EXPECT_STREQ(cmd_name(CMD_ACK), "ACK");
    EXPECT_STREQ(cmd_name(0xFF), "UNKNOWN");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
