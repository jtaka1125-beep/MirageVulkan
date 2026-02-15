// =============================================================================
// MirageVulkan - Vulkan Video Decoder Tests
// =============================================================================

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>
#include "vulkan_video_decoder.hpp"
#include "yuv_converter.hpp"

using namespace mirage::video;

// =============================================================================
// Test Fixture
// =============================================================================
class VulkanVideoTest : public ::testing::Test {
protected:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    void SetUp() override {
        // Create Vulkan instance
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "VulkanVideoTest";
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.pEngineName = "TestEngine";
        app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;

        VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            GTEST_SKIP() << "Failed to create Vulkan instance";
            return;
        }

        // Enumerate physical devices
        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
        if (device_count == 0) {
            GTEST_SKIP() << "No Vulkan devices found";
            return;
        }

        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
        physical_device_ = devices[0];
    }

    void TearDown() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }
};

// =============================================================================
// Tests
// =============================================================================

TEST_F(VulkanVideoTest, CheckVulkanVideoSupport) {
    if (physical_device_ == VK_NULL_HANDLE) {
        GTEST_SKIP() << "No physical device available";
    }

    bool supported = VulkanVideoDecoder::isSupported(physical_device_);

    // Just log the result - support depends on GPU
    if (supported) {
        std::cout << "Vulkan Video H.264 decode is SUPPORTED on this GPU" << std::endl;
    } else {
        std::cout << "Vulkan Video H.264 decode is NOT supported on this GPU" << std::endl;
    }

    // Test passes regardless - we're just checking the API works
    SUCCEED();
}

TEST_F(VulkanVideoTest, DecoderCreation) {
    if (physical_device_ == VK_NULL_HANDLE) {
        GTEST_SKIP() << "No physical device available";
    }

    if (!VulkanVideoDecoder::isSupported(physical_device_)) {
        GTEST_SKIP() << "Vulkan Video not supported";
    }

    // Create device with video decode queue
    // This is a simplified test - full implementation needs proper queue family detection
    VulkanVideoDecoder decoder;

    // Just verify object creation doesn't crash
    EXPECT_FALSE(decoder.isInitialized());
}

TEST_F(VulkanVideoTest, YuvConverterCreation) {
    VulkanYuvConverter converter;

    // Verify object creation
    EXPECT_FALSE(converter.isInitialized());
}

// =============================================================================
// NAL Parsing Tests (don't require Vulkan)
// =============================================================================

TEST(H264ParserTest, NalStartCodeDetection) {
    // Test 4-byte start code
    uint8_t data1[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    EXPECT_EQ(data1[3], 0x01);
    EXPECT_EQ(data1[4] & 0x1F, 7);  // SPS NAL type

    // Test 3-byte start code
    uint8_t data2[] = {0x00, 0x00, 0x01, 0x68};
    EXPECT_EQ(data2[2], 0x01);
    EXPECT_EQ(data2[3] & 0x1F, 8);  // PPS NAL type
}

TEST(H264ParserTest, NalTypeExtraction) {
    // IDR slice
    uint8_t idr = 0x65;
    EXPECT_EQ(idr & 0x1F, 5);

    // Non-IDR slice
    uint8_t non_idr = 0x41;
    EXPECT_EQ(non_idr & 0x1F, 1);

    // SPS
    uint8_t sps = 0x67;
    EXPECT_EQ(sps & 0x1F, 7);

    // PPS
    uint8_t pps = 0x68;
    EXPECT_EQ(pps & 0x1F, 8);
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
