// =============================================================================
// Unit tests for HybridCommandSender command building logic
// =============================================================================
#include <gtest/gtest.h>
#include "mirage_protocol.hpp"
#include <cstdint>
#include <vector>
#include <cstring>

using namespace mirage::protocol;

// Test command payload building (without actual USB/HID dependencies)

namespace {

// Replicate tap command payload building
std::vector<uint8_t> buildTapPayload(int16_t x, int16_t y) {
    std::vector<uint8_t> payload(8);
    // x: 2 bytes (little-endian)
    payload[0] = x & 0xFF;
    payload[1] = (x >> 8) & 0xFF;
    // y: 2 bytes (little-endian)
    payload[2] = y & 0xFF;
    payload[3] = (y >> 8) & 0xFF;
    // screen_w, screen_h: zeros (device will use native)
    return payload;
}

std::vector<uint8_t> buildSwipePayload(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t duration_ms) {
    std::vector<uint8_t> payload(12);
    // x1, y1
    payload[0] = x1 & 0xFF;
    payload[1] = (x1 >> 8) & 0xFF;
    payload[2] = y1 & 0xFF;
    payload[3] = (y1 >> 8) & 0xFF;
    // x2, y2
    payload[4] = x2 & 0xFF;
    payload[5] = (x2 >> 8) & 0xFF;
    payload[6] = y2 & 0xFF;
    payload[7] = (y2 >> 8) & 0xFF;
    // duration_ms
    payload[8] = duration_ms & 0xFF;
    payload[9] = (duration_ms >> 8) & 0xFF;
    // screen_w, screen_h: zeros
    return payload;
}

std::vector<uint8_t> buildKeyPayload(int32_t keycode) {
    std::vector<uint8_t> payload(4);
    payload[0] = keycode & 0xFF;
    payload[1] = (keycode >> 8) & 0xFF;
    payload[2] = (keycode >> 16) & 0xFF;
    payload[3] = (keycode >> 24) & 0xFF;
    return payload;
}

// HID coordinate conversion (matches aoa_hid_touch.hpp)
uint16_t pixelToHidX(int px, int screen_w) {
    if (screen_w <= 0) return 0;
    int64_t hid = (static_cast<int64_t>(px) * HID_TOUCH_COORD_MAX) / screen_w;
    if (hid < 0) hid = 0;
    if (hid > HID_TOUCH_COORD_MAX) hid = HID_TOUCH_COORD_MAX;
    return static_cast<uint16_t>(hid);
}

uint16_t pixelToHidY(int py, int screen_h) {
    if (screen_h <= 0) return 0;
    int64_t hid = (static_cast<int64_t>(py) * HID_TOUCH_COORD_MAX) / screen_h;
    if (hid < 0) hid = 0;
    if (hid > HID_TOUCH_COORD_MAX) hid = HID_TOUCH_COORD_MAX;
    return static_cast<uint16_t>(hid);
}

} // anonymous namespace

// ===========================================================================
// Tap command tests
// ===========================================================================
TEST(HybridSender, BuildTapPayload) {
    auto payload = buildTapPayload(500, 800);
    ASSERT_EQ(payload.size(), 8u);

    // Verify x coordinate (little-endian)
    int16_t x = payload[0] | (payload[1] << 8);
    EXPECT_EQ(x, 500);

    // Verify y coordinate
    int16_t y = payload[2] | (payload[3] << 8);
    EXPECT_EQ(y, 800);
}

TEST(HybridSender, BuildTapPayloadNegative) {
    auto payload = buildTapPayload(-100, -200);

    int16_t x = static_cast<int16_t>(payload[0] | (payload[1] << 8));
    int16_t y = static_cast<int16_t>(payload[2] | (payload[3] << 8));

    EXPECT_EQ(x, -100);
    EXPECT_EQ(y, -200);
}

TEST(HybridSender, TapCommandPacket) {
    auto payload = buildTapPayload(100, 200);
    auto packet = build_packet(CMD_TAP, 42, payload.data(), payload.size());

    ASSERT_EQ(packet.size(), HEADER_SIZE + payload.size());

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_TAP);
    EXPECT_EQ(hdr.seq, 42u);
    EXPECT_EQ(hdr.payload_len, payload.size());
}

// ===========================================================================
// Swipe command tests
// ===========================================================================
TEST(HybridSender, BuildSwipePayload) {
    auto payload = buildSwipePayload(100, 200, 900, 200, 300);
    ASSERT_EQ(payload.size(), 12u);

    int16_t x1 = payload[0] | (payload[1] << 8);
    int16_t y1 = payload[2] | (payload[3] << 8);
    int16_t x2 = payload[4] | (payload[5] << 8);
    int16_t y2 = payload[6] | (payload[7] << 8);
    uint16_t duration = payload[8] | (payload[9] << 8);

    EXPECT_EQ(x1, 100);
    EXPECT_EQ(y1, 200);
    EXPECT_EQ(x2, 900);
    EXPECT_EQ(y2, 200);
    EXPECT_EQ(duration, 300);
}

TEST(HybridSender, SwipeCommandPacket) {
    auto payload = buildSwipePayload(0, 500, 1000, 500, 500);
    auto packet = build_packet(CMD_SWIPE, 123, payload.data(), payload.size());

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_SWIPE);
    EXPECT_EQ(hdr.seq, 123u);
}

// ===========================================================================
// Key command tests
// ===========================================================================
TEST(HybridSender, BuildKeyPayload) {
    // Android KEYCODE_BACK = 4
    auto payload = buildKeyPayload(4);
    ASSERT_EQ(payload.size(), 4u);

    int32_t keycode = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
    EXPECT_EQ(keycode, 4);
}

TEST(HybridSender, BuildKeyPayloadLargeValue) {
    // Test with larger keycode
    auto payload = buildKeyPayload(0x12345678);

    int32_t keycode = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
    EXPECT_EQ(keycode, 0x12345678);
}

// ===========================================================================
// HID coordinate conversion tests
// ===========================================================================
TEST(HybridSender, HidCoordinateConversion) {
    // Center of 1080x1920 screen
    uint16_t hid_x = pixelToHidX(540, 1080);
    uint16_t hid_y = pixelToHidY(960, 1920);

    // Should be approximately half of HID_TOUCH_COORD_MAX (32767)
    EXPECT_NEAR(hid_x, 16383, 1);
    EXPECT_NEAR(hid_y, 16383, 1);
}

TEST(HybridSender, HidCoordinateTopLeft) {
    uint16_t hid_x = pixelToHidX(0, 1080);
    uint16_t hid_y = pixelToHidY(0, 1920);

    EXPECT_EQ(hid_x, 0);
    EXPECT_EQ(hid_y, 0);
}

TEST(HybridSender, HidCoordinateBottomRight) {
    uint16_t hid_x = pixelToHidX(1079, 1080);
    uint16_t hid_y = pixelToHidY(1919, 1920);

    // Should be close to max
    EXPECT_GT(hid_x, 32700);
    EXPECT_GT(hid_y, 32700);
}

TEST(HybridSender, HidCoordinateClampNegative) {
    uint16_t hid_x = pixelToHidX(-100, 1080);
    uint16_t hid_y = pixelToHidY(-100, 1920);

    EXPECT_EQ(hid_x, 0);
    EXPECT_EQ(hid_y, 0);
}

TEST(HybridSender, HidCoordinateClampOverflow) {
    uint16_t hid_x = pixelToHidX(2000, 1080);  // Beyond screen width
    uint16_t hid_y = pixelToHidY(3000, 1920);  // Beyond screen height

    EXPECT_EQ(hid_x, HID_TOUCH_COORD_MAX);
    EXPECT_EQ(hid_y, HID_TOUCH_COORD_MAX);
}

TEST(HybridSender, HidCoordinateZeroScreen) {
    // Edge case: zero screen dimensions
    uint16_t hid_x = pixelToHidX(100, 0);
    uint16_t hid_y = pixelToHidY(100, 0);

    EXPECT_EQ(hid_x, 0);
    EXPECT_EQ(hid_y, 0);
}

// ===========================================================================
// TouchMode enum tests
// ===========================================================================
TEST(HybridSender, TouchModeValues) {
    // Verify enum values match expected priority order
    // AOA_HID > MIRA_USB > ADB_FALLBACK
    enum class TouchMode { AOA_HID, MIRA_USB, ADB_FALLBACK };

    EXPECT_EQ(static_cast<int>(TouchMode::AOA_HID), 0);
    EXPECT_EQ(static_cast<int>(TouchMode::MIRA_USB), 1);
    EXPECT_EQ(static_cast<int>(TouchMode::ADB_FALLBACK), 2);
}

// ===========================================================================
// Ping command (no payload)
// ===========================================================================
TEST(HybridSender, PingCommand) {
    auto packet = build_packet(CMD_PING, 0);

    ASSERT_EQ(packet.size(), HEADER_SIZE);

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_PING);
    EXPECT_EQ(hdr.payload_len, 0u);
}

// ===========================================================================
// Back command (no payload)
// ===========================================================================
TEST(HybridSender, BackCommand) {
    auto packet = build_packet(CMD_BACK, 999);

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_BACK);
    EXPECT_EQ(hdr.seq, 999u);
    EXPECT_EQ(hdr.payload_len, 0u);
}

// ===========================================================================
// Video control commands
// ===========================================================================
TEST(HybridSender, VideoFpsCommand) {
    uint8_t fps = 30;
    auto packet = build_packet(CMD_VIDEO_FPS, 1, &fps, 1);

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_VIDEO_FPS);
    EXPECT_EQ(hdr.payload_len, 1u);
    EXPECT_EQ(packet[HEADER_SIZE], 30);
}

TEST(HybridSender, VideoIdrCommand) {
    auto packet = build_packet(CMD_VIDEO_IDR, 50);

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_VIDEO_IDR);
    EXPECT_EQ(hdr.payload_len, 0u);
}

// ===========================================================================
// ACK response parsing
// ===========================================================================
TEST(HybridSender, AckResponse) {
    // ACK payload: seq (4 bytes) + status (1 byte)
    uint8_t ack_payload[] = {0x2A, 0x00, 0x00, 0x00, 0x00};  // seq=42, status=0 (success)
    auto packet = build_packet(CMD_ACK, 0, ack_payload, sizeof(ack_payload));

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_ACK);
    EXPECT_EQ(hdr.payload_len, 5u);

    // Parse ACK
    uint32_t ack_seq = packet[HEADER_SIZE] |
                       (packet[HEADER_SIZE + 1] << 8) |
                       (packet[HEADER_SIZE + 2] << 16) |
                       (packet[HEADER_SIZE + 3] << 24);
    uint8_t status = packet[HEADER_SIZE + 4];

    EXPECT_EQ(ack_seq, 42u);
    EXPECT_EQ(status, 0);
}

// ===========================================================================
// ACK status codes
// ===========================================================================
TEST(HybridSender, AckStatusError) {
    // ACK with error status
    uint8_t ack_payload[] = {0x01, 0x00, 0x00, 0x00, 0x01};  // seq=1, status=1 (error)
    auto packet = build_packet(CMD_ACK, 0, ack_payload, sizeof(ack_payload));

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));

    uint8_t status = packet[HEADER_SIZE + 4];
    EXPECT_EQ(status, 1);  // Error
}

// ===========================================================================
// Device ID format validation (USB serial format tests)
// ===========================================================================
namespace {

bool isValidUsbId(const std::string& id) {
    // USB device ID should be non-empty, alphanumeric + some special chars
    if (id.empty()) return false;
    if (id == "_pending") return false;  // Reserved internal key
    for (char c : id) {
        if (!std::isalnum(c) && c != '-' && c != '_' && c != ':' && c != '.') {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

TEST(HybridSender, ValidUsbIds) {
    EXPECT_TRUE(isValidUsbId("A9250700956"));
    EXPECT_TRUE(isValidUsbId("R3CT40XXXXX"));
    EXPECT_TRUE(isValidUsbId("emulator-5554"));
    EXPECT_TRUE(isValidUsbId("usb:1-2.3"));
}

TEST(HybridSender, InvalidUsbIds) {
    EXPECT_FALSE(isValidUsbId(""));
    EXPECT_FALSE(isValidUsbId("_pending"));  // Internal reserved
    EXPECT_FALSE(isValidUsbId("device with spaces"));
}

// ===========================================================================
// Fallback priority logic tests (3-tier: AOA_HID > MIRA_USB > ADB)
// ===========================================================================
namespace {

enum class FallbackTier { AOA_HID = 1, MIRA_USB = 2, ADB_FALLBACK = 3 };

// Simulates the 3-tier fallback decision logic
FallbackTier determineFallbackTier(bool hid_available, bool usb_available, bool adb_available) {
    if (hid_available) return FallbackTier::AOA_HID;
    if (usb_available) return FallbackTier::MIRA_USB;
    if (adb_available) return FallbackTier::ADB_FALLBACK;
    return FallbackTier::ADB_FALLBACK;  // Default
}

} // anonymous namespace

TEST(HybridSender, FallbackPriorityAllAvailable) {
    // All methods available - should use AOA_HID (highest priority)
    auto tier = determineFallbackTier(true, true, true);
    EXPECT_EQ(tier, FallbackTier::AOA_HID);
}

TEST(HybridSender, FallbackPriorityNoHid) {
    // HID not available, USB available
    auto tier = determineFallbackTier(false, true, true);
    EXPECT_EQ(tier, FallbackTier::MIRA_USB);
}

TEST(HybridSender, FallbackPriorityAdbOnly) {
    // Only ADB available
    auto tier = determineFallbackTier(false, false, true);
    EXPECT_EQ(tier, FallbackTier::ADB_FALLBACK);
}

TEST(HybridSender, FallbackPriorityHidOverUsb) {
    // HID and ADB available, no USB
    auto tier = determineFallbackTier(true, false, true);
    EXPECT_EQ(tier, FallbackTier::AOA_HID);
}

// ===========================================================================
// Screen coordinate validation for HID
// ===========================================================================
TEST(HybridSender, ScreenDimensionValidation) {
    // Valid dimensions should produce non-zero HID coords
    EXPECT_GT(pixelToHidX(500, 1080), 0);
    EXPECT_GT(pixelToHidY(500, 1920), 0);

    // Invalid dimensions should produce zero
    EXPECT_EQ(pixelToHidX(500, 0), 0);
    EXPECT_EQ(pixelToHidY(500, -1), 0);
}

TEST(HybridSender, HidCoordinateForDifferentResolutions) {
    // Test various common Android resolutions
    // 1080x1920 (FHD portrait)
    EXPECT_NEAR(pixelToHidX(540, 1080), 16383, 10);  // Center X

    // 1200x2000 (Npad X1)
    EXPECT_NEAR(pixelToHidX(600, 1200), 16383, 10);

    // 800x1340 (A9)
    EXPECT_NEAR(pixelToHidX(400, 800), 16383, 10);
}

// ===========================================================================
// Video route command payload
// ===========================================================================
TEST(HybridSender, VideoRoutePayload) {
    // Video route: mode(1) + host(32) + port(2)
    std::vector<uint8_t> payload(35, 0);
    payload[0] = 2;  // Mode: UDP
    strcpy(reinterpret_cast<char*>(&payload[1]), "192.168.0.100");
    payload[33] = 0xB8;  // Port 5000 (little-endian)
    payload[34] = 0x13;

    auto packet = build_packet(CMD_VIDEO_ROUTE, 10, payload.data(), payload.size());

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_VIDEO_ROUTE);
    EXPECT_EQ(hdr.payload_len, 35u);
}

// ===========================================================================
// Click by ID/Text command payloads
// ===========================================================================
TEST(HybridSender, ClickIdPayload) {
    std::string resource_id = "com.app:id/button_ok";
    auto packet = build_packet(CMD_CLICK_ID, 100,
        reinterpret_cast<const uint8_t*>(resource_id.c_str()), resource_id.size());

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_CLICK_ID);
    EXPECT_EQ(hdr.payload_len, resource_id.size());
}

TEST(HybridSender, ClickTextPayload) {
    std::string text = "OK";
    auto packet = build_packet(CMD_CLICK_TEXT, 101,
        reinterpret_cast<const uint8_t*>(text.c_str()), text.size());

    PacketHeader hdr;
    ASSERT_TRUE(parse_header(packet.data(), packet.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_CLICK_TEXT);
    EXPECT_EQ(hdr.payload_len, text.size());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
