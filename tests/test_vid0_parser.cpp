// =============================================================================
// Unit tests for VID0 packet parser (src/vid0_parser.hpp)
// =============================================================================
#include <gtest/gtest.h>
#include "vid0_parser.hpp"

using namespace mirage::video;

// Helper: build a valid VID0 packet (MAGIC + LENGTH + payload)
static std::vector<uint8_t> makeVid0Packet(const std::vector<uint8_t>& rtp_payload) {
    std::vector<uint8_t> pkt;
    uint32_t magic = VID0_MAGIC;
    uint32_t len = static_cast<uint32_t>(rtp_payload.size());

    // Big-endian magic
    pkt.push_back(static_cast<uint8_t>((magic >> 24) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((magic >> 16) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((magic >>  8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((magic      ) & 0xFF));

    // Big-endian length
    pkt.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((len >>  8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((len      ) & 0xFF));

    pkt.insert(pkt.end(), rtp_payload.begin(), rtp_payload.end());
    return pkt;
}

// Helper: create a minimal valid RTP payload (>= RTP_MIN_LEN = 12 bytes)
static std::vector<uint8_t> makeMinimalRtp() {
    return std::vector<uint8_t>(12, 0xAA);
}

// ---------------------------------------------------------------------------
// Empty buffer returns no packets
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, EmptyBuffer) {
    std::vector<uint8_t> buffer;
    auto result = parseVid0Packets(buffer);

    EXPECT_TRUE(result.rtp_packets.empty());
    EXPECT_EQ(result.sync_errors, 0);
    EXPECT_FALSE(result.buffer_overflow);
    EXPECT_TRUE(buffer.empty());
}

// ---------------------------------------------------------------------------
// Single valid packet
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, SingleValidPacket) {
    auto rtp = makeMinimalRtp();
    auto pkt = makeVid0Packet(rtp);
    std::vector<uint8_t> buffer = pkt;

    auto result = parseVid0Packets(buffer);

    ASSERT_EQ(result.rtp_packets.size(), 1u);
    EXPECT_EQ(result.rtp_packets[0], rtp);
    EXPECT_EQ(result.sync_errors, 0);
    EXPECT_TRUE(buffer.empty()); // fully consumed
}

// ---------------------------------------------------------------------------
// Multiple consecutive packets
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, MultiplePackets) {
    auto rtp1 = std::vector<uint8_t>(20, 0x11);
    auto rtp2 = std::vector<uint8_t>(30, 0x22);

    auto pkt1 = makeVid0Packet(rtp1);
    auto pkt2 = makeVid0Packet(rtp2);

    std::vector<uint8_t> buffer;
    buffer.insert(buffer.end(), pkt1.begin(), pkt1.end());
    buffer.insert(buffer.end(), pkt2.begin(), pkt2.end());

    auto result = parseVid0Packets(buffer);

    ASSERT_EQ(result.rtp_packets.size(), 2u);
    EXPECT_EQ(result.rtp_packets[0], rtp1);
    EXPECT_EQ(result.rtp_packets[1], rtp2);
    EXPECT_TRUE(buffer.empty());
}

// ---------------------------------------------------------------------------
// Incomplete packet (not enough data for payload) - stays in buffer
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, IncompletePacket) {
    auto rtp = makeMinimalRtp();
    auto pkt = makeVid0Packet(rtp);
    // Truncate: remove last 4 bytes
    pkt.resize(pkt.size() - 4);

    std::vector<uint8_t> buffer = pkt;
    size_t original_size = buffer.size();

    auto result = parseVid0Packets(buffer);

    EXPECT_TRUE(result.rtp_packets.empty());
    EXPECT_EQ(buffer.size(), original_size); // data preserved for next read
}

// ---------------------------------------------------------------------------
// Garbage before valid packet -> sync error + recovery
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, SyncRecovery) {
    std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF};
    auto rtp = makeMinimalRtp();
    auto pkt = makeVid0Packet(rtp);

    std::vector<uint8_t> buffer;
    buffer.insert(buffer.end(), garbage.begin(), garbage.end());
    buffer.insert(buffer.end(), pkt.begin(), pkt.end());

    auto result = parseVid0Packets(buffer);

    ASSERT_EQ(result.rtp_packets.size(), 1u);
    EXPECT_EQ(result.rtp_packets[0], rtp);
    EXPECT_GT(result.sync_errors, 0);
}

// ---------------------------------------------------------------------------
// Packet with invalid length (too small) is skipped
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, InvalidLengthTooSmall) {
    // Build a VID0 header with length < RTP_MIN_LEN (12)
    std::vector<uint8_t> buffer;
    uint32_t magic = VID0_MAGIC;
    buffer.push_back(static_cast<uint8_t>((magic >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((magic >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((magic >>  8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((magic      ) & 0xFF));
    // Length = 4 (too small)
    buffer.push_back(0x00);
    buffer.push_back(0x00);
    buffer.push_back(0x00);
    buffer.push_back(0x04);
    buffer.resize(buffer.size() + 4, 0x00); // payload

    // Append a valid packet after
    auto rtp = makeMinimalRtp();
    auto valid = makeVid0Packet(rtp);
    buffer.insert(buffer.end(), valid.begin(), valid.end());

    auto result = parseVid0Packets(buffer);

    // The valid packet should still be parsed
    ASSERT_EQ(result.rtp_packets.size(), 1u);
    EXPECT_EQ(result.rtp_packets[0], rtp);
}

// ---------------------------------------------------------------------------
// Buffer overflow protection constants are sane
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, BufferOverflowConstants) {
    // The overflow guard (buffer.size() > BUFFER_MAX) is a safety net that fires
    // when accumulated unparsed data exceeds 128KB between parse calls. Since
    // RTP_MAX_LEN (65535) < BUFFER_MAX, a single incomplete frame can never alone
    // trigger overflow. Verify the constants are correctly related.
    EXPECT_EQ(BUFFER_MAX, 128u * 1024u);
    EXPECT_EQ(BUFFER_TRIM, 32u * 1024u);
    EXPECT_LT(BUFFER_TRIM, BUFFER_MAX);
    EXPECT_LT(RTP_MAX_LEN + VID0_HEADER_SIZE, BUFFER_MAX);
}

// ---------------------------------------------------------------------------
// Large buffer with parseable data is fully consumed
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, LargeBufferFullyConsumed) {
    std::vector<uint8_t> buffer;
    auto rtp = makeMinimalRtp();

    // Fill ~130KB of valid VID0 packets
    while (buffer.size() < BUFFER_MAX + 1024) {
        auto pkt = makeVid0Packet(rtp);
        buffer.insert(buffer.end(), pkt.begin(), pkt.end());
    }

    auto result = parseVid0Packets(buffer);

    EXPECT_GT(result.rtp_packets.size(), 100u);
    EXPECT_FALSE(result.buffer_overflow);
    EXPECT_LT(buffer.size(), VID0_HEADER_SIZE + RTP_MIN_LEN); // residual < 1 packet
}

// ---------------------------------------------------------------------------
// Only header present (no payload bytes yet)
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, HeaderOnlyNoPayload) {
    std::vector<uint8_t> buffer;
    uint32_t magic = VID0_MAGIC;
    buffer.push_back(static_cast<uint8_t>((magic >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((magic >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((magic >>  8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((magic      ) & 0xFF));
    // length = 20
    buffer.push_back(0x00);
    buffer.push_back(0x00);
    buffer.push_back(0x00);
    buffer.push_back(0x14);

    size_t original_size = buffer.size();
    auto result = parseVid0Packets(buffer);

    EXPECT_TRUE(result.rtp_packets.empty());
    EXPECT_EQ(buffer.size(), original_size); // retained for next append
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
