// =============================================================================
// Unit tests for VID0 packet parser (src/vid0_parser.hpp)
// Header-only, no network/ADB dependencies
// VID0 format: [MAGIC(4,BE)] [LENGTH(4,BE)] [RTP_DATA(LENGTH)]
// MAGIC = 0x56494430 ("VID0")
// =============================================================================
#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include "vid0_parser.hpp"

using namespace mirage::video;

// ---------------------------------------------------------------------------
// Helper: build a single VID0-framed packet
// ---------------------------------------------------------------------------
static std::vector<uint8_t> makeVid0(const std::vector<uint8_t>& rtp) {
    uint32_t len = static_cast<uint32_t>(rtp.size());
    std::vector<uint8_t> pkt;
    // MAGIC
    pkt.push_back(0x56); pkt.push_back(0x49); pkt.push_back(0x44); pkt.push_back(0x30);
    // LENGTH big-endian
    pkt.push_back((len >> 24) & 0xFF);
    pkt.push_back((len >> 16) & 0xFF);
    pkt.push_back((len >>  8) & 0xFF);
    pkt.push_back( len        & 0xFF);
    pkt.insert(pkt.end(), rtp.begin(), rtp.end());
    return pkt;
}

// Minimal valid RTP payload (12 bytes minimum)
static std::vector<uint8_t> minRtp() { return std::vector<uint8_t>(12, 0xAB); }

// RTP of given size
static std::vector<uint8_t> rtpOfSize(size_t n) { return std::vector<uint8_t>(n, 0x55); }

// ---------------------------------------------------------------------------
// V-1: Empty buffer → no packets, no errors
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, EmptyBuffer) {
    std::vector<uint8_t> buf;
    auto r = parseVid0Packets(buf);
    EXPECT_TRUE(r.rtp_packets.empty());
    EXPECT_EQ(r.sync_errors, 0);
    EXPECT_FALSE(r.buffer_overflow);
    EXPECT_TRUE(buf.empty());  // nothing consumed from empty
}

// ---------------------------------------------------------------------------
// V-2: Single valid packet fully present → extracted
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, SingleValidPacket) {
    auto rtp = minRtp();
    auto buf = makeVid0(rtp);
    auto r = parseVid0Packets(buf);
    ASSERT_EQ(r.rtp_packets.size(), 1u);
    EXPECT_EQ(r.rtp_packets[0], rtp);
    EXPECT_EQ(r.sync_errors, 0);
    EXPECT_TRUE(buf.empty());  // fully consumed
}

// ---------------------------------------------------------------------------
// V-3: Three consecutive valid packets → all extracted
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, MultiplePackets) {
    auto rtp1 = rtpOfSize(100);
    auto rtp2 = rtpOfSize(200);
    auto rtp3 = rtpOfSize(50);
    auto buf = makeVid0(rtp1);
    auto p2  = makeVid0(rtp2);
    auto p3  = makeVid0(rtp3);
    buf.insert(buf.end(), p2.begin(), p2.end());
    buf.insert(buf.end(), p3.begin(), p3.end());

    auto r = parseVid0Packets(buf);
    ASSERT_EQ(r.rtp_packets.size(), 3u);
    EXPECT_EQ(r.rtp_packets[0], rtp1);
    EXPECT_EQ(r.rtp_packets[1], rtp2);
    EXPECT_EQ(r.rtp_packets[2], rtp3);
    EXPECT_EQ(r.sync_errors, 0);
    EXPECT_TRUE(buf.empty());
}

// ---------------------------------------------------------------------------
// V-4: Incomplete packet (header present, data truncated) → 0 packets, buffer kept
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, IncompletePacketWaits) {
    auto rtp = rtpOfSize(200);
    auto full = makeVid0(rtp);
    // Truncate to header + first 50 bytes of payload
    std::vector<uint8_t> partial(full.begin(), full.begin() + VID0_HEADER_SIZE + 50);

    auto r = parseVid0Packets(partial);
    EXPECT_EQ(r.rtp_packets.size(), 0u);
    EXPECT_EQ(r.sync_errors, 0);
    // Buffer should retain at least the header bytes (not consumed)
    EXPECT_GE(partial.size(), VID0_HEADER_SIZE);
}

// ---------------------------------------------------------------------------
// V-5: Only magic header, no length bytes → 0 packets
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, OnlyMagicNoLength) {
    std::vector<uint8_t> buf = {0x56, 0x49, 0x44, 0x30};
    auto r = parseVid0Packets(buf);
    EXPECT_EQ(r.rtp_packets.size(), 0u);
}

// ---------------------------------------------------------------------------
// V-6: Garbage prefix → sync error, then valid packet recovered
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, GarbagePrefixResync) {
    std::vector<uint8_t> buf = {0xFF, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};  // garbage
    auto valid = makeVid0(minRtp());
    buf.insert(buf.end(), valid.begin(), valid.end());

    auto r = parseVid0Packets(buf);
    EXPECT_EQ(r.rtp_packets.size(), 1u);
    EXPECT_GT(r.sync_errors, 0);
    EXPECT_GT(r.magic_resync, 0);
    EXPECT_TRUE(buf.empty());
}

// ---------------------------------------------------------------------------
// V-7: Two garbage bytes then two valid packets
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, GarbageThenTwoPackets) {
    std::vector<uint8_t> buf = {0x01, 0x02};
    auto p1 = makeVid0(rtpOfSize(12));
    auto p2 = makeVid0(rtpOfSize(20));
    buf.insert(buf.end(), p1.begin(), p1.end());
    buf.insert(buf.end(), p2.begin(), p2.end());

    auto r = parseVid0Packets(buf);
    EXPECT_EQ(r.rtp_packets.size(), 2u);
    EXPECT_GT(r.sync_errors, 0);
    EXPECT_TRUE(buf.empty());
}

// ---------------------------------------------------------------------------
// V-8: Length too small (< RTP_MIN_LEN=12) → invalid_len count incremented
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, InvalidLengthTooSmall) {
    std::vector<uint8_t> buf;
    // MAGIC + length=5 (< 12)
    buf = {0x56, 0x49, 0x44, 0x30,  0x00, 0x00, 0x00, 0x05};
    buf.insert(buf.end(), 5, 0xAA);  // payload

    auto r = parseVid0Packets(buf);
    EXPECT_EQ(r.rtp_packets.size(), 0u);
    EXPECT_GT(r.invalid_len, 0);
}

// ---------------------------------------------------------------------------
// V-9: Length > RTP_MAX_LEN (65535) → invalid_len count incremented
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, InvalidLengthTooLarge) {
    std::vector<uint8_t> buf;
    // MAGIC + length = 65536 (> 65535)
    uint32_t too_big = 65536;
    buf = {0x56, 0x49, 0x44, 0x30,
           uint8_t(too_big >> 24), uint8_t(too_big >> 16),
           uint8_t(too_big >>  8), uint8_t(too_big & 0xFF)};

    auto r = parseVid0Packets(buf);
    EXPECT_EQ(r.rtp_packets.size(), 0u);
    EXPECT_GT(r.invalid_len, 0);
}

// ---------------------------------------------------------------------------
// V-10: Exact minimum RTP size (12 bytes) → accepted
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, MinimumRtpSizeAccepted) {
    auto rtp = rtpOfSize(12);  // exactly RTP_MIN_LEN
    auto buf = makeVid0(rtp);
    auto r = parseVid0Packets(buf);
    ASSERT_EQ(r.rtp_packets.size(), 1u);
    EXPECT_EQ(r.rtp_packets[0].size(), 12u);
}

// ---------------------------------------------------------------------------
// V-11: Exact maximum RTP size (65535 bytes) → accepted
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, MaximumRtpSizeAccepted) {
    auto rtp = rtpOfSize(65535);  // exactly RTP_MAX_LEN
    auto buf = makeVid0(rtp);
    auto r = parseVid0Packets(buf);
    ASSERT_EQ(r.rtp_packets.size(), 1u);
    EXPECT_EQ(r.rtp_packets[0].size(), 65535u);
    EXPECT_TRUE(buf.empty());
}

// ---------------------------------------------------------------------------
// V-12: Buffer consumed correctly — remainder after partial
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, BufferConsumedPartially) {
    auto rtp = minRtp();
    auto p1 = makeVid0(rtp);
    // p2 is incomplete: only header
    std::vector<uint8_t> buf(p1.begin(), p1.end());
    buf.push_back(0x56); buf.push_back(0x49); buf.push_back(0x44); buf.push_back(0x30);
    // No length bytes for p2

    auto r = parseVid0Packets(buf);
    EXPECT_EQ(r.rtp_packets.size(), 1u);
    // The partial magic should remain
    EXPECT_EQ(buf.size(), 4u);
}

// ---------------------------------------------------------------------------
// V-13: ParseResult stats accurately count each error type in one pass
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, StatsAccurate) {
    // Part 1: 3 garbage bytes (triggers sync_error + magic_resync)
    std::vector<uint8_t> buf = {0xAA, 0xBB, 0xCC};
    // Part 2: invalid length = 5 (< RTP_MIN_LEN=12) → invalid_len
    buf.push_back(0x56); buf.push_back(0x49); buf.push_back(0x44); buf.push_back(0x30);
    buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x05);
    buf.insert(buf.end(), 5, 0x00);  // fake payload bytes (< 12)
    // Part 3: valid packet → recovered
    auto valid = makeVid0(minRtp());
    buf.insert(buf.end(), valid.begin(), valid.end());

    auto r = parseVid0Packets(buf);
    EXPECT_GT(r.sync_errors, 0)     << "sync_errors should be > 0";
    EXPECT_GT(r.magic_resync, 0)    << "magic_resync should be > 0";
    EXPECT_GT(r.invalid_len, 0)     << "invalid_len should be > 0";
    EXPECT_FALSE(r.buffer_overflow) << "no overflow for small buffer";
    ASSERT_EQ(r.rtp_packets.size(), 1u) << "one valid packet should be extracted";
}

// ---------------------------------------------------------------------------
// V-14: Magic bytes in payload do NOT cause false resync
// ---------------------------------------------------------------------------
TEST(Vid0ParserTest, MagicInPayloadNoFalseResync) {
    // Build RTP payload that contains VID0 magic bytes
    std::vector<uint8_t> rtp = rtpOfSize(50);
    rtp[10] = 0x56; rtp[11] = 0x49; rtp[12] = 0x44; rtp[13] = 0x30;  // "VID0" in payload
    auto buf = makeVid0(rtp);

    auto r = parseVid0Packets(buf);
    ASSERT_EQ(r.rtp_packets.size(), 1u);
    EXPECT_EQ(r.sync_errors, 0);
    EXPECT_EQ(r.rtp_packets[0], rtp);
    EXPECT_TRUE(buf.empty());
}
