// =============================================================================
// MirageSystem - Unified Video Decoder Pipeline
// =============================================================================
// Tier 1: Vulkan Video H.264 decode (GPU zero-copy)
// Tier 2: FFmpeg fallback (D3D11VA/DXVA2 HW accel)
//
// Automatic runtime detection and fallback:
// - NVIDIA Pascal+ (GTX 10xx+): Vulkan Video
// - AMD RDNA 1+ (RX 5000+): Vulkan Video
// - Intel Iris Xe+ (11th gen+): Vulkan Video
// - All others: FFmpeg with D3D11VA
// =============================================================================
#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <functional>
#include <string>
#include <cstdint>
#include <mutex>
#include <atomic>

namespace mirage::video {

// Forward declarations
class VulkanVideoDecoder;
class VulkanYuvConverter;

// =============================================================================
// Decoder Backend Type
// =============================================================================
enum class DecoderBackend {
    Unknown,
    VulkanVideo,    // Tier 1: Full GPU pipeline
    FFmpegHW,       // Tier 2: FFmpeg with D3D11VA
    FFmpegSW,       // Tier 3: FFmpeg CPU decode (last resort)
};

// Video codec type
enum class VideoCodec {
    H264,
    HEVC,
};

// =============================================================================
// Decoded Frame
// =============================================================================
struct DecodedFrame {
    // Vulkan resources (valid when using Vulkan pipeline)
    VkImage vk_image = VK_NULL_HANDLE;
    VkImageView vk_view = VK_NULL_HANDLE;
    VkDescriptorSet vk_descriptor = VK_NULL_HANDLE;

    // CPU buffer (valid when using FFmpeg fallback)
    const uint8_t* rgba_data = nullptr;
    bool owns_data = false;

    // Common properties
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t pts = 0;
    int32_t poc = 0;

    DecoderBackend backend = DecoderBackend::Unknown;

    bool isVulkan() const { return backend == DecoderBackend::VulkanVideo; }
    bool hasData() const { return vk_image != VK_NULL_HANDLE || rgba_data != nullptr; }
};

// =============================================================================
// Frame Callback
// =============================================================================
using DecodedFrameCallback = std::function<void(const DecodedFrame& frame)>;

// =============================================================================
// Unified Decoder Configuration
// =============================================================================
struct UnifiedDecoderConfig {
    uint32_t max_width = 1920;
    uint32_t max_height = 1080;
    uint32_t dpb_slot_count = 8;

    VideoCodec codec = VideoCodec::H264;

    bool prefer_vulkan_video = true;    // Try Vulkan Video first
    bool allow_ffmpeg_fallback = true;  // Fall back to FFmpeg if Vulkan unavailable
    bool enable_hw_accel = true;        // Use D3D11VA in FFmpeg fallback

    // Vulkan resources (required for Vulkan Video)
    VkInstance instance = VK_NULL_HANDLE;  // Required for Vulkan Video
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    uint32_t graphics_queue_family = 0;
    VkQueue video_decode_queue = VK_NULL_HANDLE;
    uint32_t video_decode_queue_family = 0;
    VkQueue compute_queue = VK_NULL_HANDLE;
    uint32_t compute_queue_family = 0;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
};

// =============================================================================
// UnifiedDecoder - Automatic backend selection and fallback
// =============================================================================
class UnifiedDecoder {
public:
    UnifiedDecoder();
    ~UnifiedDecoder();

    // Non-copyable
    UnifiedDecoder(const UnifiedDecoder&) = delete;
    UnifiedDecoder& operator=(const UnifiedDecoder&) = delete;

    // Initialize with configuration
    bool initialize(const UnifiedDecoderConfig& config);

    // Decode H.264 NAL unit (Annex-B format)
    // Returns true if frame was decoded, frame data available via callback
    bool decode(const uint8_t* nal_data, size_t nal_size, int64_t pts = 0);

    // Decode complete access unit (may contain multiple NALs)
    int decodeAccessUnit(const uint8_t* data, size_t size, int64_t pts = 0);

    // Flush decoder (output all buffered frames)
    int flush();

    // Set frame callback
    void setFrameCallback(DecodedFrameCallback callback);

    // Get current backend
    DecoderBackend backend() const { return backend_; }
    const char* backendName() const;

    // Get current resolution
    uint32_t width() const { return current_width_; }
    uint32_t height() const { return current_height_; }

    // Statistics
    uint64_t framesDecoded() const { return frames_decoded_; }
    uint64_t errorsCount() const { return errors_count_; }

    // Check backend availability
    static bool isVulkanVideoAvailable(VkPhysicalDevice physical_device);

    // Cleanup
    void destroy();

private:
    bool initializeVulkanVideo();
    bool initializeFFmpeg();

    void onVulkanFrame(VkImage nv12_image, VkImageView nv12_view,
                       uint32_t width, uint32_t height, int64_t pts);
    void onFFmpegFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height, int64_t pts);

    UnifiedDecoderConfig config_;
    DecoderBackend backend_ = DecoderBackend::Unknown;

    // Vulkan Video (Tier 1)
    std::unique_ptr<VulkanVideoDecoder> vulkan_decoder_;
    std::unique_ptr<VulkanYuvConverter> yuv_converter_;

    // NV12 plane views for YUV conversion
    VkImageView y_plane_view_ = VK_NULL_HANDLE;
    VkImageView uv_plane_view_ = VK_NULL_HANDLE;
    VkImage current_nv12_image_ = VK_NULL_HANDLE;
    uint32_t plane_view_width_ = 0;
    uint32_t plane_view_height_ = 0;

    // Helper methods
    bool createPlaneViews(VkImage nv12_image, uint32_t width, uint32_t height);
    void destroyPlaneViews();

    // FFmpeg fallback (Tier 2/3)
    // Note: We reuse the existing H264Decoder from MirageComplete
    struct FFmpegDecoder;
    std::unique_ptr<FFmpegDecoder> ffmpeg_decoder_;

    // User callback
    DecodedFrameCallback frame_callback_;

    // Current state
    uint32_t current_width_ = 0;
    uint32_t current_height_ = 0;

    // Statistics
    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> errors_count_{0};

    // Thread safety
    std::mutex decode_mutex_;

    bool initialized_ = false;
};

} // namespace mirage::video
