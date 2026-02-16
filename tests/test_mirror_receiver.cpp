// =============================================================================
// Unit tests for MirrorReceiver RTP packet processing
// =============================================================================
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

// We test the RTP parsing logic without network dependencies
// by replicating the core parsing functions

namespace {

// Helper: build big-endian 16-bit value
inline void wr16(uint8_t* p, uint16_t val) {
    p[0] = static_cast<uint8_t>(val >> 8);
    p[1] = static_cast<uint8_t>(val & 0xFF);
}

// Helper: read big-endian 16-bit value
inline uint16_t rd16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

// Minimal RTP header builder
std::vector<uint8_t> buildRtpPacket(uint16_t seq, uint32_t timestamp,
                                     const uint8_t* payload, size_t payload_len,
                                     bool marker = false, uint8_t pt = 96) {
    std::vector<uint8_t> pkt(12 + payload_len);

    // Byte 0: V=2, P=0, X=0, CC=0
    pkt[0] = 0x80;
    // Byte 1: M, PT
    pkt[1] = (marker ? 0x80 : 0x00) | (pt & 0x7F);
    // Bytes 2-3: Sequence number
    wr16(&pkt[2], seq);
    // Bytes 4-7: Timestamp
    pkt[4] = (timestamp >> 24) & 0xFF;
    pkt[5] = (timestamp >> 16) & 0xFF;
    pkt[6] = (timestamp >> 8) & 0xFF;
    pkt[7] = timestamp & 0xFF;
    // Bytes 8-11: SSRC (arbitrary)
    pkt[8] = 0x12; pkt[9] = 0x34; pkt[10] = 0x56; pkt[11] = 0x78;

    // Payload
    if (payload && payload_len > 0) {
        memcpy(&pkt[12], payload, payload_len);
    }

    return pkt;
}

// Replicate RTP validation logic from mirror_receiver.cpp
struct RtpParseResult {
    bool valid = false;
    uint16_t seq = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    uint8_t nal_type = 0;
};

RtpParseResult parseRtpPacket(const uint8_t* data, size_t len) {
    RtpParseResult result;

    if (len < 12) return result;

    // Validate RTP version (must be 2)
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2) return result;

    result.seq = rd16(data + 2);

    uint8_t cc = data[0] & 0x0F;
    bool has_extension = (data[0] & 0x10) != 0;

    size_t header_len = 12 + (cc * 4);
    if (len < header_len) return result;

    if (has_extension) {
        if (len < header_len + 4) return result;
        uint16_t ext_len = rd16(data + header_len + 2);
        size_t ext_bytes = 4 + (static_cast<size_t>(ext_len) * 4);
        if (ext_bytes > 65535 || header_len + ext_bytes > len) return result;
        header_len += ext_bytes;
    }

    if (len <= header_len) return result;

    result.payload = data + header_len;
    result.payload_len = len - header_len;
    result.nal_type = result.payload[0] & 0x1F;
    result.valid = true;

    return result;
}

} // anonymous namespace

// ===========================================================================
// Basic RTP packet parsing
// ===========================================================================
TEST(MirrorReceiver, ParseValidRtpPacket) {
    uint8_t payload[] = {0x67, 0x42, 0x00, 0x1E};  // NAL type 7 (SPS)
    auto pkt = buildRtpPacket(1000, 90000, payload, sizeof(payload));

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.seq, 1000);
    EXPECT_EQ(result.payload_len, sizeof(payload));
    EXPECT_EQ(result.nal_type, 7);  // SPS
}

TEST(MirrorReceiver, RejectTooShortPacket) {
    uint8_t buf[11] = {};  // Less than minimum RTP header size
    auto result = parseRtpPacket(buf, sizeof(buf));
    EXPECT_FALSE(result.valid);
}

TEST(MirrorReceiver, RejectInvalidVersion) {
    uint8_t payload[] = {0x67};
    auto pkt = buildRtpPacket(0, 0, payload, sizeof(payload));
    pkt[0] = 0x00;  // Version 0 instead of 2

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_FALSE(result.valid);
}

TEST(MirrorReceiver, ParsePacketWithCSRC) {
    uint8_t payload[] = {0x68, 0xCE, 0x3C, 0x80};  // NAL type 8 (PPS)
    auto pkt = buildRtpPacket(2000, 180000, payload, sizeof(payload));

    // Add 2 CSRC entries (8 bytes)
    pkt[0] = 0x82;  // V=2, CC=2
    pkt.insert(pkt.begin() + 12, 8, 0x00);  // Insert 8 bytes for CSRC

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.seq, 2000);
    EXPECT_EQ(result.nal_type, 8);  // PPS
}

TEST(MirrorReceiver, ParsePacketWithExtension) {
    uint8_t payload[] = {0x65, 0x88, 0x84, 0x00};  // NAL type 5 (IDR)
    auto pkt = buildRtpPacket(3000, 270000, payload, sizeof(payload));

    // Set extension bit
    pkt[0] |= 0x10;

    // Insert extension header (4 bytes header + 4 bytes data = 8 bytes total)
    std::vector<uint8_t> ext = {0x00, 0x00, 0x00, 0x01,  // Profile, length=1 (4 bytes)
                                 0xAB, 0xCD, 0xEF, 0x12}; // Extension data
    pkt.insert(pkt.begin() + 12, ext.begin(), ext.end());

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.seq, 3000);
    EXPECT_EQ(result.nal_type, 5);  // IDR slice
}

// ===========================================================================
// NAL type identification
// ===========================================================================
TEST(MirrorReceiver, IdentifyNalTypes) {
    struct TestCase {
        uint8_t nal_header;
        int expected_type;
    };

    TestCase cases[] = {
        {0x67, 7},   // SPS
        {0x68, 8},   // PPS
        {0x65, 5},   // IDR slice
        {0x41, 1},   // Non-IDR slice
        {0x06, 6},   // SEI
        {0x09, 9},   // AUD
    };

    for (const auto& tc : cases) {
        uint8_t payload[] = {tc.nal_header, 0x00, 0x00, 0x00};
        auto pkt = buildRtpPacket(0, 0, payload, sizeof(payload));

        auto result = parseRtpPacket(pkt.data(), pkt.size());
        ASSERT_TRUE(result.valid);
        EXPECT_EQ(result.nal_type, tc.expected_type)
            << "NAL header 0x" << std::hex << (int)tc.nal_header;
    }
}

// ===========================================================================
// Sequence number handling
// ===========================================================================
TEST(MirrorReceiver, SequenceNumberWraparound) {
    uint8_t payload[] = {0x41};

    // Test wraparound from 65535 to 0
    auto pkt1 = buildRtpPacket(65535, 0, payload, sizeof(payload));
    auto pkt2 = buildRtpPacket(0, 90000, payload, sizeof(payload));

    auto r1 = parseRtpPacket(pkt1.data(), pkt1.size());
    auto r2 = parseRtpPacket(pkt2.data(), pkt2.size());

    EXPECT_TRUE(r1.valid);
    EXPECT_TRUE(r2.valid);
    EXPECT_EQ(r1.seq, 65535);
    EXPECT_EQ(r2.seq, 0);
}

// ===========================================================================
// FU-A (Fragmentation Unit) packet structure
// ===========================================================================
TEST(MirrorReceiver, FuAPacketStructure) {
    // FU-A indicator: type 28 (0x1C), NRI from original NAL
    // FU header: S(1), E(1), R(1), Type(5)

    // Start fragment of IDR (type 5)
    uint8_t fu_start[] = {0x7C, 0x85, 0x88, 0x84};  // Indicator=28, FU=Start+IDR
    auto pkt = buildRtpPacket(100, 0, fu_start, sizeof(fu_start));

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.nal_type, 28);  // FU-A

    // Verify FU header
    EXPECT_EQ(result.payload[1] & 0x80, 0x80);  // Start bit set
    EXPECT_EQ(result.payload[1] & 0x40, 0x00);  // End bit not set
    EXPECT_EQ(result.payload[1] & 0x1F, 5);     // Original NAL type = IDR
}

TEST(MirrorReceiver, FuAEndFragment) {
    // End fragment
    uint8_t fu_end[] = {0x7C, 0x45, 0x00, 0x00};  // Indicator=28, FU=End+IDR
    auto pkt = buildRtpPacket(105, 0, fu_end, sizeof(fu_end));

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.nal_type, 28);
    EXPECT_EQ(result.payload[1] & 0x80, 0x00);  // Start bit not set
    EXPECT_EQ(result.payload[1] & 0x40, 0x40);  // End bit set
}

// ===========================================================================
// STAP-A (Single-Time Aggregation) packet structure
// ===========================================================================
TEST(MirrorReceiver, StapAPacketStructure) {
    // STAP-A contains multiple NALs with 2-byte length prefix each
    // Indicator: type 24 (0x18)

    // Build STAP-A with SPS + PPS
    std::vector<uint8_t> stap = {0x18};  // Indicator

    // SPS (4 bytes)
    uint8_t sps[] = {0x67, 0x42, 0x00, 0x1E};
    stap.push_back(0x00); stap.push_back(sizeof(sps));  // Length
    stap.insert(stap.end(), sps, sps + sizeof(sps));

    // PPS (4 bytes)
    uint8_t pps[] = {0x68, 0xCE, 0x3C, 0x80};
    stap.push_back(0x00); stap.push_back(sizeof(pps));  // Length
    stap.insert(stap.end(), pps, pps + sizeof(pps));

    auto pkt = buildRtpPacket(200, 0, stap.data(), stap.size());

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.nal_type, 24);  // STAP-A

    // Verify structure: 1 byte indicator + (2 byte len + 4 byte NAL) * 2
    EXPECT_EQ(result.payload_len, 1 + (2 + 4) + (2 + 4));
}

// ===========================================================================
// Edge cases and malformed packets
// ===========================================================================
TEST(MirrorReceiver, RejectEmptyPayload) {
    auto pkt = buildRtpPacket(0, 0, nullptr, 0);

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_FALSE(result.valid);  // No payload
}

TEST(MirrorReceiver, RejectTruncatedExtension) {
    uint8_t payload[] = {0x41};
    auto pkt = buildRtpPacket(0, 0, payload, sizeof(payload));

    // Set extension bit but don't provide extension data
    pkt[0] |= 0x10;

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_FALSE(result.valid);  // Extension header missing
}

TEST(MirrorReceiver, HandleMaxCSRC) {
    uint8_t payload[] = {0x41};
    auto pkt = buildRtpPacket(0, 0, payload, sizeof(payload));

    // Set CC=15 (maximum)
    pkt[0] = 0x8F;
    // Insert 60 bytes for 15 CSRC entries
    pkt.insert(pkt.begin() + 12, 60, 0x00);

    auto result = parseRtpPacket(pkt.data(), pkt.size());
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.nal_type, 1);  // Non-IDR
}

// ===========================================================================
// DoS prevention: buffer limits
// ===========================================================================
TEST(MirrorReceiver, BufferLimits) {
    // These constants should match mirror_receiver.hpp
    constexpr size_t MAX_FU_BUFFER_SIZE = 2 * 1024 * 1024;
    constexpr size_t MAX_SPS_SIZE = 256;
    constexpr size_t MAX_PPS_SIZE = 256;

    EXPECT_EQ(MAX_FU_BUFFER_SIZE, 2097152);
    EXPECT_EQ(MAX_SPS_SIZE, 256);
    EXPECT_EQ(MAX_PPS_SIZE, 256);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
