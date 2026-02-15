// =============================================================================
// MirageSystem - Vulkan YUV to RGBA Converter
// =============================================================================
// GPU-based NV12 to RGBA conversion using compute shaders
// Zero-copy: works directly on Vulkan Video decoder output
// =============================================================================
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <mutex>

namespace mirage::video {

// =============================================================================
// Color Space
// =============================================================================
enum class ColorSpace : uint32_t {
    BT601 = 0,  // SDTV (standard definition)
    BT709 = 1,  // HDTV (high definition)
};

// =============================================================================
// YUV Converter Configuration
// =============================================================================
struct YuvConverterConfig {
    uint32_t max_width = 1920;
    uint32_t max_height = 1080;
    ColorSpace color_space = ColorSpace::BT709;
};

// =============================================================================
// VulkanYuvConverter - NV12 to RGBA compute shader converter
// =============================================================================
class VulkanYuvConverter {
public:
    VulkanYuvConverter();
    ~VulkanYuvConverter();

    // Non-copyable
    VulkanYuvConverter(const VulkanYuvConverter&) = delete;
    VulkanYuvConverter& operator=(const VulkanYuvConverter&) = delete;

    // Initialize with Vulkan device
    bool initialize(VkDevice device,
                    VkPhysicalDevice physical_device,
                    uint32_t compute_queue_family,
                    VkQueue compute_queue,
                    const YuvConverterConfig& config = {});

    // Convert NV12 VkImage to RGBA VkImage
    // Input: NV12 image from Vulkan Video decoder
    // Output: RGBA image for display/processing
    bool convert(VkImage nv12_input,
                 VkImageView y_view,      // Y plane view
                 VkImageView uv_view,     // UV plane view
                 uint32_t width,
                 uint32_t height,
                 VkImage rgba_output,
                 VkImageView rgba_view);

    // Convert with semaphore synchronization
    bool convertAsync(VkImage nv12_input,
                      VkImageView y_view,
                      VkImageView uv_view,
                      uint32_t width,
                      uint32_t height,
                      VkImage rgba_output,
                      VkImageView rgba_view,
                      VkSemaphore wait_semaphore,
                      VkSemaphore signal_semaphore);

    // Get output RGBA image (for cases where we manage the output)
    VkImage outputImage() const { return output_image_; }
    VkImageView outputView() const { return output_view_; }
    VkDescriptorSet outputDescriptorSet() const { return output_ds_; }

    // Create managed output image
    bool createOutputImage(uint32_t width, uint32_t height);

    // Cleanup
    void destroy();

    // Check if initialized
    bool isInitialized() const { return initialized_; }

private:
    bool createPipeline();
    bool createDescriptorPool();
    bool createSampler();

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    uint32_t compute_queue_family_ = 0;

    // Compute pipeline
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkShaderModule shader_module_ = VK_NULL_HANDLE;

    // Descriptors
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    // Sampler for NV12 planes
    VkSampler sampler_ = VK_NULL_HANDLE;

    // Command pool/buffer
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buffer_ = VK_NULL_HANDLE;

    // Managed output image (optional)
    VkImage output_image_ = VK_NULL_HANDLE;
    VkImageView output_view_ = VK_NULL_HANDLE;
    VkDeviceMemory output_memory_ = VK_NULL_HANDLE;
    VkDescriptorSet output_ds_ = VK_NULL_HANDLE;

    // Configuration
    YuvConverterConfig config_;
    uint32_t current_width_ = 0;
    uint32_t current_height_ = 0;

    // Thread safety
    std::mutex convert_mutex_;

    bool initialized_ = false;
};

} // namespace mirage::video
