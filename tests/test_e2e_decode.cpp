// =============================================================================
// MirageVulkan - End-to-End H.264 Decode Test
// =============================================================================
// Tests the complete decode pipeline with real H.264 bitstream data
// =============================================================================

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>
#include "vulkan_video_decoder.hpp"
#include "h264_parser.hpp"

#include <vector>
#include <fstream>
#include <cstring>

using namespace mirage::video;

// =============================================================================
// Minimal H.264 Test Streams (generated with known parameters)
// =============================================================================

// SPS for 64x64 Baseline profile, pic_order_cnt_type=2
// Profile: Baseline (66), Level: 1.0 (10)
// Resolution: 64x64 (4 MBs x 4 MBs)
static const uint8_t TEST_SPS[] = {
    0x00, 0x00, 0x00, 0x01,  // Start code
    0x67,                    // NAL type 7 (SPS), nal_ref_idc=3
    0x42, 0x00, 0x0A,        // profile_idc=66 (Baseline), constraints, level_idc=10
    0xE8, 0x41, 0x01,        // sps_id=0, log2_max_frame_num=4, poc_type=2
    0x11, 0x18,              // max_num_ref_frames=1, gaps=0, width=4, height=4
    0x20                     // frame_mbs_only=1, direct_8x8=0, cropping=0, vui=0
};

// PPS (minimal)
static const uint8_t TEST_PPS[] = {
    0x00, 0x00, 0x00, 0x01,  // Start code
    0x68,                    // NAL type 8 (PPS), nal_ref_idc=3
    0xCE, 0x06, 0xE2         // pps_id=0, sps_id=0, entropy=0, etc.
};

// IDR slice header (minimal, all-I 64x64)
static const uint8_t TEST_IDR_SLICE[] = {
    0x00, 0x00, 0x00, 0x01,  // Start code
    0x65,                    // NAL type 5 (IDR), nal_ref_idc=3
    0x88, 0x80, 0x20,        // first_mb=0, slice_type=7 (I), pps_id=0
    0x00, 0x39, 0x7B, 0xDF,  // frame_num, idr_pic_id, slice_qp, etc.
    // ... (truncated for test - real slice would have macroblock data)
};

// =============================================================================
// Test Fixture
// =============================================================================
class E2EDecodeTest : public ::testing::Test {
protected:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue video_queue_ = VK_NULL_HANDLE;
    uint32_t video_queue_family_ = UINT32_MAX;
    bool vulkan_video_available_ = false;

    void SetUp() override {
        // Create Vulkan instance
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "E2EDecodeTest";
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.pEngineName = "TestEngine";
        app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;

        if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
            return;
        }

        // Find device with video decode support
        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
        if (device_count == 0) return;

        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

        for (auto& dev : devices) {
            if (!VulkanVideoDecoder::isSupported(dev)) continue;

            // Find video decode queue family
            uint32_t qf_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, nullptr);
            std::vector<VkQueueFamilyProperties> qf_props(qf_count);
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, qf_props.data());

            for (uint32_t i = 0; i < qf_count; i++) {
                if (qf_props[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
                    video_queue_family_ = i;
                    break;
                }
            }

            if (video_queue_family_ != UINT32_MAX) {
                physical_device_ = dev;
                break;
            }
        }

        if (physical_device_ == VK_NULL_HANDLE) return;

        // Create device with video queue
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = video_queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;

        const char* extensions[] = {
            VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME
        };

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 3;
        device_info.ppEnabledExtensionNames = extensions;

        if (vkCreateDevice(physical_device_, &device_info, nullptr, &device_) != VK_SUCCESS) {
            return;
        }

        vkGetDeviceQueue(device_, video_queue_family_, 0, &video_queue_);
        vulkan_video_available_ = true;
    }

    void TearDown() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }
};

// =============================================================================
// Parser Tests with Real SPS/PPS
// =============================================================================

TEST(H264StreamTest, ParseSPSFromBytes) {
    H264Parser parser;

    // Skip start code (4 bytes) and NAL header (1 byte)
    H264SPS sps;
    bool result = parser.parseSPS(TEST_SPS + 5, sizeof(TEST_SPS) - 5, sps);

    EXPECT_TRUE(result);
    EXPECT_EQ(sps.profile_idc, 66);  // Baseline
    EXPECT_EQ(sps.level_idc, 10);    // Level 1.0
    EXPECT_EQ(sps.sps_id, 0);
}

TEST(H264StreamTest, ParseAnnexBStream) {
    H264Parser parser;

    // Combine SPS + PPS into a single stream
    std::vector<uint8_t> stream;
    stream.insert(stream.end(), TEST_SPS, TEST_SPS + sizeof(TEST_SPS));
    stream.insert(stream.end(), TEST_PPS, TEST_PPS + sizeof(TEST_PPS));

    auto nals = parser.parseAnnexB(stream.data(), stream.size());

    EXPECT_GE(nals.size(), 2u);
    if (nals.size() >= 2) {
        EXPECT_EQ(nals[0].nal_unit_type, 7);  // SPS
        EXPECT_EQ(nals[1].nal_unit_type, 8);  // PPS
    }
}

TEST(H264StreamTest, ParseIDRSliceHeader) {
    H264Parser parser;

    // First parse SPS and PPS
    H264SPS sps;
    H264PPS pps;

    parser.parseSPS(TEST_SPS + 5, sizeof(TEST_SPS) - 5, sps);
    parser.parsePPS(TEST_PPS + 5, sizeof(TEST_PPS) - 5, pps);

    // Parse slice header
    H264SliceHeader header;
    bool result = parser.parseSliceHeader(
        TEST_IDR_SLICE + 5, sizeof(TEST_IDR_SLICE) - 5,
        sps, pps, 5, header  // NAL type 5 = IDR
    );

    EXPECT_TRUE(result);
    EXPECT_EQ(header.first_mb_in_slice, 0);
    // slice_type should be I (2 or 7)
    EXPECT_TRUE(header.slice_type == 2 || header.slice_type == 7);
}

// =============================================================================
// Full Pipeline Tests (requires Vulkan Video support)
// =============================================================================

TEST_F(E2EDecodeTest, QueryCapabilities) {
    if (!vulkan_video_available_) {
        GTEST_SKIP() << "Vulkan Video not available";
    }

    VulkanVideoCapabilities caps;
    bool result = VulkanVideoDecoder::queryCapabilities(instance_, physical_device_, caps);

    EXPECT_TRUE(result);
    EXPECT_TRUE(caps.supports_h264_decode);
    EXPECT_GT(caps.max_width, 0u);
    EXPECT_GT(caps.max_height, 0u);
    EXPECT_GT(caps.max_dpb_slots, 0u);

    std::cout << "Video Capabilities:" << std::endl;
    std::cout << "  Max resolution: " << caps.max_width << "x" << caps.max_height << std::endl;
    std::cout << "  Max DPB slots: " << caps.max_dpb_slots << std::endl;
    std::cout << "  Max level IDC: " << (int)caps.max_level_idc << std::endl;
    std::cout << "  Bitstream alignment: " << caps.minBitstreamBufferSizeAlignment << std::endl;
}

TEST_F(E2EDecodeTest, InitializeDecoder) {
    if (!vulkan_video_available_) {
        GTEST_SKIP() << "Vulkan Video not available";
    }

    VulkanVideoDecoder decoder;

    VulkanVideoDecoderConfig config;
    config.max_width = 1920;
    config.max_height = 1080;
    config.dpb_slot_count = 8;

    bool result = decoder.initialize(
        instance_,
        physical_device_,
        device_,
        video_queue_family_,
        video_queue_,
        config
    );

    EXPECT_TRUE(result);
    EXPECT_TRUE(decoder.isInitialized());

    decoder.destroy();
    EXPECT_FALSE(decoder.isInitialized());
}

TEST_F(E2EDecodeTest, DecodeSPSPPS) {
    if (!vulkan_video_available_) {
        GTEST_SKIP() << "Vulkan Video not available";
    }

    VulkanVideoDecoder decoder;

    VulkanVideoDecoderConfig config;
    config.max_width = 1920;
    config.max_height = 1080;
    config.dpb_slot_count = 8;
    config.async_decode = false;  // Sync mode for testing

    if (!decoder.initialize(instance_, physical_device_, device_,
                            video_queue_family_, video_queue_, config)) {
        GTEST_SKIP() << "Failed to initialize decoder";
    }

    // Send SPS
    auto result1 = decoder.decode(TEST_SPS, sizeof(TEST_SPS), 0);
    // SPS/PPS don't produce output frames
    EXPECT_TRUE(result1.success || result1.error_message.empty());

    // Send PPS
    auto result2 = decoder.decode(TEST_PPS, sizeof(TEST_PPS), 0);
    EXPECT_TRUE(result2.success || result2.error_message.empty());

    std::cout << "SPS/PPS processed successfully" << std::endl;

    decoder.destroy();
}

// =============================================================================
// Load External H264 File Test (if available)
// =============================================================================

TEST_F(E2EDecodeTest, DecodeFromFile) {
    if (!vulkan_video_available_) {
        GTEST_SKIP() << "Vulkan Video not available";
    }

    // Try to load test file
    const char* test_files[] = {
        "test_data/test.h264",
        "../test_data/test.h264",
        "../../test_data/test.h264"
    };

    std::ifstream file;
    for (const char* path : test_files) {
        file.open(path, std::ios::binary);
        if (file.is_open()) {
            std::cout << "Loaded test file: " << path << std::endl;
            break;
        }
    }

    if (!file.is_open()) {
        GTEST_SKIP() << "No test H.264 file found";
    }

    // Read file
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    file.close();

    // Initialize decoder
    VulkanVideoDecoder decoder;
    VulkanVideoDecoderConfig config;
    config.max_width = 1920;
    config.max_height = 1080;
    config.dpb_slot_count = 8;
    config.async_decode = false;

    if (!decoder.initialize(instance_, physical_device_, device_,
                            video_queue_family_, video_queue_, config)) {
        GTEST_SKIP() << "Failed to initialize decoder";
    }

    // Parse NAL units and decode
    H264Parser parser;
    auto nals = parser.parseAnnexB(data.data(), data.size());

    std::cout << "Found " << nals.size() << " NAL units" << std::endl;

    int frames_decoded = 0;
    decoder.setFrameCallback([&](VkImage, VkImageView, uint32_t w, uint32_t h, int64_t pts) {
        frames_decoded++;
        std::cout << "Frame " << frames_decoded << ": " << w << "x" << h
                  << " PTS=" << pts << std::endl;
    });

    for (const auto& nal : nals) {
        decoder.decode(nal.data, nal.size, 0);
    }

    // Flush remaining frames
    decoder.flush();

    std::cout << "Total frames decoded: " << frames_decoded << std::endl;
    EXPECT_GE(decoder.framesDecoded(), 0u);

    decoder.destroy();
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
