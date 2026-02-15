#pragma once

#include "vulkan/vulkan_context.hpp"
#include "vulkan/vulkan_compute.hpp"
#include "vulkan/vulkan_image.hpp"
#include <memory>
#include <string>

namespace mirage::vk {

/**
 * High-level Vulkan Compute processor for image operations.
 * 
 * Replaces OpenCL + OpenCV pipeline in AIEngine:
 *   Before: cv::cvtColor(RGBA→Gray) → clCreateImage → OpenCL template match
 *   After:  VulkanImage(RGBA) → Compute Shader (RGBA→Gray) → VulkanImage(Gray)
 *
 * NOT thread-safe - caller must synchronize. Uses its own command pool and fence for async operations.
 */
class VulkanComputeProcessor {
public:
    VulkanComputeProcessor() = default;
    ~VulkanComputeProcessor() { shutdown(); }

    VulkanComputeProcessor(const VulkanComputeProcessor&) = delete;
    VulkanComputeProcessor& operator=(const VulkanComputeProcessor&) = delete;

    /**
     * Initialize with an existing VulkanContext.
     * Creates compute command pool and loads shaders.
     * @param ctx           Vulkan context (must outlive this object)
     * @param shader_dir    Directory containing .spv files
     */
    bool initialize(VulkanContext& ctx, const std::string& shader_dir);

    void shutdown();

    /**
     * Convert RGBA frame to grayscale on GPU.
     * @param rgba      Input RGBA data (width * height * 4 bytes)
     * @param width     Image width
     * @param height    Image height
     * @param out_gray  Output grayscale buffer (width * height bytes)
     * @return true on success
     */
    bool rgbaToGray(const uint8_t* rgba, int width, int height,
                    uint8_t* out_gray);

    /**
     * Convert RGBA frame to grayscale, keeping result on GPU.
     * Returns VulkanImage in GENERAL layout, ready for compute.
     * Caller must NOT delete the returned pointer (owned by processor).
     */
    VulkanImage* rgbaToGrayGpu(const uint8_t* rgba, int width, int height);

    bool valid() const { return initialized_; }

    // Stats
    struct Stats {
        uint64_t conversions = 0;
        double   last_time_ms = 0.0;
        double   avg_time_ms = 0.0;
    };
    Stats getStats() const { return stats_; }

private:
    bool ensureImages(int width, int height);

    VulkanContext* ctx_ = nullptr;
    bool initialized_ = false;

    // Compute command pool (separate from graphics)
    VkCommandPool  cmd_pool_ = VK_NULL_HANDLE;
    VkFence        fence_    = VK_NULL_HANDLE;

    // RGBA→Gray pipeline
    std::unique_ptr<VulkanComputePipeline> gray_pipeline_;
    VkDescriptorSet gray_ds_ = VK_NULL_HANDLE;

    // Reusable images (re-created if resolution changes)
    std::unique_ptr<VulkanImage> input_rgba_;
    std::unique_ptr<VulkanImage> output_gray_;
    int current_width_ = 0;
    int current_height_ = 0;

    Stats stats_;
};

} // namespace mirage::vk
