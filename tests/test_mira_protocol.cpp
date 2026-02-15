// =============================================================================
// Unit tests for MIRA protocol (src/mirage_protocol.hpp)
// =============================================================================
#include <gtest/gtest.h>
#include "mirage_protocol.hpp"

using namespace mirage::protocol;

// ===========================================================================
// build_header / parse_header round-trip
// ===========================================================================
TEST(MiraProtocol, BuildAndParseHeaderRoundTrip) {
    uint8_t buf[HEADER_SIZE];
    build_header(buf, CMD_TAP, 42, 8);

    PacketHeader hdr{};
    ASSERT_TRUE(parse_header(buf, HEADER_SIZE, hdr));
    EXPECT_EQ(hdr.magic, PROTOCOL_MAGIC);
    EXPECT_EQ(hdr.version, PROTOCOL_VERSION);
    EXPECT_EQ(hdr.cmd, CMD_TAP);
    EXPECT_EQ(hdr.seq, 42u);
    EXPECT_EQ(hdr.payload_len, 8u);
}

TEST(MiraProtocol, BuildHeaderReturnsPastEnd) {
    uint8_t buf[HEADER_SIZE + 4];
    uint8_t* end = build_header(buf, CMD_PING, 0, 0);
    EXPECT_EQ(end, buf + HEADER_SIZE);
}

// ===========================================================================
// parse_header validation
// ===========================================================================
TEST(MiraProtocol, ParseHeaderTooShort) {
    uint8_t buf[HEADER_SIZE - 1] = {};
    PacketHeader hdr{};
    EXPECT_FALSE(parse_header(buf, sizeof(buf), hdr));
}

TEST(MiraProtocol, ParseHeaderBadMagic) {
    uint8_t buf[HEADER_SIZE] = {};
    // Write wrong magic
    uint32_t bad_magic = 0xDEADBEEF;
    memcpy(buf, &bad_magic, 4);
    buf[4] = PROTOCOL_VERSION;

    PacketHeader hdr{};
    EXPECT_FALSE(parse_header(buf, HEADER_SIZE, hdr));
}

TEST(MiraProtocol, ParseHeaderPayloadTooLarge) {
    uint8_t buf[HEADER_SIZE];
    // payload_len > MAX_PAYLOAD should fail
    build_header(buf, CMD_TAP, 1, MAX_PAYLOAD + 1);

    PacketHeader hdr{};
    EXPECT_FALSE(parse_header(buf, HEADER_SIZE, hdr));
}

TEST(MiraProtocol, ParseHeaderMaxPayloadAccepted) {
    uint8_t buf[HEADER_SIZE];
    build_header(buf, CMD_TAP, 1, MAX_PAYLOAD);

    PacketHeader hdr{};
    ASSERT_TRUE(parse_header(buf, HEADER_SIZE, hdr));
    EXPECT_EQ(hdr.payload_len, MAX_PAYLOAD);
}

TEST(MiraProtocol, ParseHeaderZeroPayload) {
    uint8_t buf[HEADER_SIZE];
    build_header(buf, CMD_PING, 0, 0);

    PacketHeader hdr{};
    ASSERT_TRUE(parse_header(buf, HEADER_SIZE, hdr));
    EXPECT_EQ(hdr.payload_len, 0u);
}

// ===========================================================================
// build_packet
// ===========================================================================
TEST(MiraProtocol, BuildPacketNoPayload) {
    auto pkt = build_packet(CMD_PING, 100);
    ASSERT_EQ(pkt.size(), HEADER_SIZE);

    PacketHeader hdr{};
    ASSERT_TRUE(parse_header(pkt.data(), pkt.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_PING);
    EXPECT_EQ(hdr.seq, 100u);
    EXPECT_EQ(hdr.payload_len, 0u);
}

TEST(MiraProtocol, BuildPacketWithPayload) {
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    auto pkt = build_packet(CMD_TAP, 7, payload, sizeof(payload));

    ASSERT_EQ(pkt.size(), HEADER_SIZE + sizeof(payload));

    PacketHeader hdr{};
    ASSERT_TRUE(parse_header(pkt.data(), pkt.size(), hdr));
    EXPECT_EQ(hdr.cmd, CMD_TAP);
    EXPECT_EQ(hdr.seq, 7u);
    EXPECT_EQ(hdr.payload_len, 4u);

    // Verify payload bytes
    EXPECT_EQ(pkt[HEADER_SIZE + 0], 0x01);
    EXPECT_EQ(pkt[HEADER_SIZE + 1], 0x02);
    EXPECT_EQ(pkt[HEADER_SIZE + 2], 0x03);
    EXPECT_EQ(pkt[HEADER_SIZE + 3], 0x04);
}

TEST(MiraProtocol, BuildPacketNullPayloadZeroLen) {
    auto pkt = build_packet(CMD_BACK, 1, nullptr, 0);
    ASSERT_EQ(pkt.size(), HEADER_SIZE);
}

// ===========================================================================
// All command types round-trip
// ===========================================================================
TEST(MiraProtocol, AllCommandTypesRoundTrip) {
    const uint8_t cmds[] = {
        CMD_PING, CMD_TAP, CMD_BACK, CMD_KEY, CMD_CONFIG,
        CMD_CLICK_ID, CMD_CLICK_TEXT, CMD_SWIPE,
        CMD_VIDEO_FPS, CMD_VIDEO_ROUTE, CMD_VIDEO_IDR, CMD_DEVICE_INFO,
        CMD_AUDIO_FRAME, CMD_ACK
    };

    for (uint8_t cmd : cmds) {
        uint8_t buf[HEADER_SIZE];
        build_header(buf, cmd, 0, 0);

        PacketHeader hdr{};
        ASSERT_TRUE(parse_header(buf, HEADER_SIZE, hdr)) << "cmd=" << (int)cmd;
        EXPECT_EQ(hdr.cmd, cmd);
    }
}

// ===========================================================================
// Sequence number wrapping
// ===========================================================================
TEST(MiraProtocol, SequenceNumberMaxValue) {
    uint8_t buf[HEADER_SIZE];
    build_header(buf, CMD_PING, UINT32_MAX, 0);

    PacketHeader hdr{};
    ASSERT_TRUE(parse_header(buf, HEADER_SIZE, hdr));
    EXPECT_EQ(hdr.seq, UINT32_MAX);
}

// ===========================================================================
// AOA PID helpers
// ===========================================================================
TEST(MiraProtocol, IsAoaPid) {
    EXPECT_TRUE(is_aoa_pid(AOA_PID_ACCESSORY_ADB));
    EXPECT_TRUE(is_aoa_pid(AOA_PID_ACCESSORY));
    EXPECT_TRUE(is_aoa_pid(AOA_PID_AUDIO));
    EXPECT_TRUE(is_aoa_pid(AOA_PID_AUDIO_ADB));
    EXPECT_TRUE(is_aoa_pid(AOA_PID_ACCESSORY_AUDIO));
    EXPECT_TRUE(is_aoa_pid(AOA_PID_ACCESSORY_AUDIO_ADB));

    // Non-AOA PIDs
    EXPECT_FALSE(is_aoa_pid(0x0000));
    EXPECT_FALSE(is_aoa_pid(0x18D1)); // Google VID, not PID
    EXPECT_FALSE(is_aoa_pid(0x2CFF));
    EXPECT_FALSE(is_aoa_pid(0x2D06)); // One past last AOA PID
}

TEST(MiraProtocol, AoaPidHasAdb) {
    EXPECT_TRUE(aoa_pid_has_adb(AOA_PID_ACCESSORY_ADB));
    EXPECT_TRUE(aoa_pid_has_adb(AOA_PID_AUDIO_ADB));
    EXPECT_TRUE(aoa_pid_has_adb(AOA_PID_ACCESSORY_AUDIO_ADB));

    EXPECT_FALSE(aoa_pid_has_adb(AOA_PID_ACCESSORY));
    EXPECT_FALSE(aoa_pid_has_adb(AOA_PID_AUDIO));
    EXPECT_FALSE(aoa_pid_has_adb(AOA_PID_ACCESSORY_AUDIO));
}

TEST(MiraProtocol, AoaPidHasAudio) {
    EXPECT_TRUE(aoa_pid_has_audio(AOA_PID_AUDIO));
    EXPECT_TRUE(aoa_pid_has_audio(AOA_PID_AUDIO_ADB));
    EXPECT_TRUE(aoa_pid_has_audio(AOA_PID_ACCESSORY_AUDIO));
    EXPECT_TRUE(aoa_pid_has_audio(AOA_PID_ACCESSORY_AUDIO_ADB));

    EXPECT_FALSE(aoa_pid_has_audio(AOA_PID_ACCESSORY));
    EXPECT_FALSE(aoa_pid_has_audio(AOA_PID_ACCESSORY_ADB));
}

// ===========================================================================
// cmd_name
// ===========================================================================
TEST(MiraProtocol, CmdNameKnown) {
    EXPECT_STREQ(cmd_name(CMD_PING), "PING");
    EXPECT_STREQ(cmd_name(CMD_TAP), "TAP");
    EXPECT_STREQ(cmd_name(CMD_BACK), "BACK");
    EXPECT_STREQ(cmd_name(CMD_KEY), "KEY");
    EXPECT_STREQ(cmd_name(CMD_SWIPE), "SWIPE");
    EXPECT_STREQ(cmd_name(CMD_ACK), "ACK");
    EXPECT_STREQ(cmd_name(CMD_VIDEO_FPS), "VIDEO_FPS");
    EXPECT_STREQ(cmd_name(CMD_VIDEO_ROUTE), "VIDEO_ROUTE");
    EXPECT_STREQ(cmd_name(CMD_VIDEO_IDR), "VIDEO_IDR");
    EXPECT_STREQ(cmd_name(CMD_AUDIO_FRAME), "AUDIO_FRAME");
}

TEST(MiraProtocol, CmdNameUnknown) {
    EXPECT_STREQ(cmd_name(0xFF), "UNKNOWN");
    EXPECT_STREQ(cmd_name(0x99), "UNKNOWN");
}

// ===========================================================================
// Protocol constants sanity
// ===========================================================================
TEST(MiraProtocol, ProtocolConstants) {
    EXPECT_EQ(PROTOCOL_MAGIC, 0x4D495241u);
    EXPECT_EQ(PROTOCOL_VERSION, 1);
    EXPECT_EQ(HEADER_SIZE, 14u);
    EXPECT_EQ(MAX_PAYLOAD, 4096u);
}

TEST(MiraProtocol, HidConstants) {
    EXPECT_EQ(HID_TOUCH_MAX_CONTACTS, 5);
    EXPECT_EQ(HID_TOUCH_COORD_MAX, 32767);
    EXPECT_EQ(HID_TOUCH_REPORT_ID, 0x01);
    EXPECT_EQ(HID_TOUCH_REPORT_SIZE, 27u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
