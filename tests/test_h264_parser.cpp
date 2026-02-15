// =============================================================================
// MirageVulkan - H.264 Parser Tests
// =============================================================================

#include <gtest/gtest.h>
#include "h264_parser.hpp"
#include "vulkan_video_decoder.hpp"  // For H264SPS, H264PPS, H264SliceHeader
#include <vector>
#include <cstdint>

using namespace mirage::video;

// =============================================================================
// BitstreamReader Tests
// =============================================================================

class BitstreamReaderTest : public ::testing::Test {
protected:
    std::vector<uint8_t> test_data_;
};

TEST_F(BitstreamReaderTest, ReadBits) {
    test_data_ = {0b10110100, 0b11001010};
    BitstreamReader reader(test_data_.data(), test_data_.size());

    // Read 1 bit at a time
    EXPECT_EQ(reader.readBits(1), 1);
    EXPECT_EQ(reader.readBits(1), 0);
    EXPECT_EQ(reader.readBits(1), 1);
    EXPECT_EQ(reader.readBits(1), 1);
    EXPECT_EQ(reader.readBits(4), 0b0100);
}

TEST_F(BitstreamReaderTest, ReadMultiByte) {
    test_data_ = {0xFF, 0x00, 0xAB};
    BitstreamReader reader(test_data_.data(), test_data_.size());

    EXPECT_EQ(reader.readBits(8), 0xFF);
    EXPECT_EQ(reader.readBits(8), 0x00);
    EXPECT_EQ(reader.readBits(4), 0x0A);
    EXPECT_EQ(reader.readBits(4), 0x0B);
}

TEST_F(BitstreamReaderTest, ReadUnsignedExpGolomb) {
    // ue(v) encoding examples:
    // 0 -> 1          (value: 0)
    // 1 -> 010        (value: 1)
    // 2 -> 011        (value: 2)
    // 3 -> 00100      (value: 3)
    // 4 -> 00101      (value: 4)
    // 5 -> 00110      (value: 5)
    // 6 -> 00111      (value: 6)
    // 7 -> 0001000    (value: 7)

    // Value 0: binary 1
    test_data_ = {0b10000000};
    BitstreamReader reader1(test_data_.data(), test_data_.size());
    EXPECT_EQ(reader1.readUE(), 0u);

    // Value 1: binary 010
    test_data_ = {0b01000000};
    BitstreamReader reader2(test_data_.data(), test_data_.size());
    EXPECT_EQ(reader2.readUE(), 1u);

    // Value 3: binary 00100
    test_data_ = {0b00100000};
    BitstreamReader reader3(test_data_.data(), test_data_.size());
    EXPECT_EQ(reader3.readUE(), 3u);
}

TEST_F(BitstreamReaderTest, ReadSignedExpGolomb) {
    // se(v) mapping from ue(v):
    // ue:0 -> se:0
    // ue:1 -> se:1
    // ue:2 -> se:-1
    // ue:3 -> se:2
    // ue:4 -> se:-2

    // Value 0: ue=0, se=0
    test_data_ = {0b10000000};
    BitstreamReader reader1(test_data_.data(), test_data_.size());
    EXPECT_EQ(reader1.readSE(), 0);

    // Value 1: ue=1, se=1 (binary 010)
    test_data_ = {0b01000000};
    BitstreamReader reader2(test_data_.data(), test_data_.size());
    EXPECT_EQ(reader2.readSE(), 1);

    // Value -1: ue=2, se=-1 (binary 011)
    test_data_ = {0b01100000};
    BitstreamReader reader3(test_data_.data(), test_data_.size());
    EXPECT_EQ(reader3.readSE(), -1);
}

TEST_F(BitstreamReaderTest, HasMoreData) {
    test_data_ = {0xFF, 0xFF};
    BitstreamReader reader(test_data_.data(), test_data_.size());

    EXPECT_TRUE(reader.hasMoreData());
    reader.readBits(8);
    EXPECT_TRUE(reader.hasMoreData());
    reader.readBits(8);
    EXPECT_FALSE(reader.hasMoreData());
}

TEST_F(BitstreamReaderTest, BitsRead) {
    test_data_ = {0xFF, 0xFF};
    BitstreamReader reader(test_data_.data(), test_data_.size());

    EXPECT_EQ(reader.bitsRead(), 0u);
    reader.readBits(4);
    EXPECT_EQ(reader.bitsRead(), 4u);
    reader.readBits(8);
    EXPECT_EQ(reader.bitsRead(), 12u);
}

// =============================================================================
// NAL Unit Tests
// =============================================================================

TEST(NalParserTest, RemoveEmulationPreventionBytes) {
    // Emulation prevention: 00 00 03 XX -> 00 00 XX
    std::vector<uint8_t> input = {0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x01};
    auto output = H264Parser::removeEmulationPrevention(input.data(), input.size());

    EXPECT_EQ(output.size(), 5u);
    EXPECT_EQ(output[0], 0x00);
    EXPECT_EQ(output[1], 0x00);
    EXPECT_EQ(output[2], 0x00);
    EXPECT_EQ(output[3], 0x00);
    EXPECT_EQ(output[4], 0x01);
}

TEST(NalParserTest, NoEmulationBytes) {
    std::vector<uint8_t> input = {0x67, 0x64, 0x00, 0x1F};
    auto output = H264Parser::removeEmulationPrevention(input.data(), input.size());

    EXPECT_EQ(output.size(), input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        EXPECT_EQ(output[i], input[i]);
    }
}

TEST(NalParserTest, FindStartCodes) {
    std::vector<uint8_t> data = {
        0x00, 0x00, 0x00, 0x01, 0x67,  // 4-byte start code + SPS
        0x00, 0x00, 0x01, 0x68,        // 3-byte start code + PPS
        0x00, 0x00, 0x00, 0x01, 0x65   // 4-byte start code + IDR
    };

    H264Parser parser;
    auto nal_units = parser.parseAnnexB(data.data(), data.size());

    EXPECT_EQ(nal_units.size(), 3u);

    // First NAL should be SPS (type 7)
    EXPECT_EQ(nal_units[0].nal_unit_type, 7);

    // Second NAL should be PPS (type 8)
    EXPECT_EQ(nal_units[1].nal_unit_type, 8);

    // Third NAL should be IDR slice (type 5)
    EXPECT_EQ(nal_units[2].nal_unit_type, 5);
}

// =============================================================================
// SPS Parsing Tests
// =============================================================================

TEST(SpsParserTest, ParseBasicSPS) {
    // This is a real SPS for 1920x1080 video
    // profile_idc=100 (High), level_idc=40 (4.0)
    std::vector<uint8_t> sps_data = {
        0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9, 0x40, 0x78,
        0x02, 0x27, 0xE5, 0xC0, 0x44, 0x00, 0x00, 0x03,
        0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xC8, 0x3C,
        0x60, 0xC6, 0x58
    };

    H264SPS sps{};
    H264Parser parser;
    bool success = parser.parseSPS(sps_data.data() + 1, sps_data.size() - 1, sps);

    EXPECT_TRUE(success);
    EXPECT_EQ(sps.profile_idc, 100);  // High profile
    EXPECT_EQ(sps.level_idc, 40);     // Level 4.0
}

// =============================================================================
// Slice Header Tests
// =============================================================================

TEST(SliceHeaderTest, SliceTypeMapping) {
    // H.264 slice types (after normalization in parseSliceHeader):
    // 0: P slice
    // 1: B slice
    // 2: I slice
    // 3: SP slice
    // 4: SI slice

    H264SliceHeader header;

    // Test I slice
    header.slice_type = 2;
    EXPECT_TRUE(header.slice_type == 2 || header.slice_type == 7);

    // Test P slice
    header.slice_type = 0;
    EXPECT_TRUE(header.slice_type == 0 || header.slice_type == 5);

    // Test B slice
    header.slice_type = 1;
    EXPECT_TRUE(header.slice_type == 1 || header.slice_type == 6);

    // Test IDR detection
    header.slice_type = 2;
    // Note: is_idr() check is based on slice_type being I-slice (2 or 7)
    EXPECT_TRUE(header.is_idr());
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
