// =============================================================================
// MirageSystem - Unified Video Decoder Pipeline Implementation
// =============================================================================

#include "unified_decoder.hpp"
#include "vulkan_video_decoder.hpp"
#include "yuv_converter.hpp"
#include "h264_decoder.hpp"  // FFmpeg fallback
#include "../mirage_log.hpp"

#include <cstring>

namespace mirage::video {

// =============================================================================
// FFmpeg Decoder Wrapper (for fallback)
// =============================================================================
struct UnifiedDecoder::FFmpegDecoder {
    std::unique_ptr<::gui::H264Decoder> decoder;
    std::vector<uint8_t> frame_buffer;

    bool init() {
        decoder = std::make_unique<::gui::H264Decoder>();
        return decoder->init();
    }

    void setCallback(std::function<void(const uint8_t*, int, int, int64_t)> cb) {
        decoder->set_frame_callback([cb](const uint8_t* data, int w, int h, uint64_t pts) {
            cb(data, w, h, static_cast<int64_t>(pts));
        });
    }

    int decode(const uint8_t* data, size_t size) {
        return decoder->decode(data, size);
    }

    int flush() {
        return decoder->flush();
    }
};

// =============================================================================
// UnifiedDecoder Implementation
// =============================================================================

UnifiedDecoder::UnifiedDecoder() = default;

UnifiedDecoder::~UnifiedDecoder() {
    destroy();
}

bool UnifiedDecoder::initialize(const UnifiedDecoderConfig& config) {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (initialized_) {
        MLOG_WARN("UnifiedDec", "Already initialized");
        return true;
    }

    config_ = config;

    // Try Vulkan Video first if configured and available
    if (config_.prefer_vulkan_video &&
        config_.instance != VK_NULL_HANDLE &&
        config_.physical_device != VK_NULL_HANDLE &&
        config_.device != VK_NULL_HANDLE) {

        if (initializeVulkanVideo()) {
            backend_ = DecoderBackend::VulkanVideo;
            initialized_ = true;
            MLOG_INFO("UnifiedDec", "Initialized with Vulkan Video backend (Tier 1)");
            return true;
        }

        MLOG_INFO("UnifiedDec", "Vulkan Video not available, trying FFmpeg fallback");
    }

    // Fall back to FFmpeg
    if (config_.allow_ffmpeg_fallback) {
        if (initializeFFmpeg()) {
            backend_ = config_.enable_hw_accel ? DecoderBackend::FFmpegHW : DecoderBackend::FFmpegSW;
            initialized_ = true;
            MLOG_INFO("UnifiedDec", "Initialized with FFmpeg %s backend (Tier %d)",
                      config_.enable_hw_accel ? "HW" : "SW",
                      config_.enable_hw_accel ? 2 : 3);
            return true;
        }
    }

    MLOG_ERROR("UnifiedDec", "Failed to initialize any decoder backend");
    return false;
}

bool UnifiedDecoder::initializeVulkanVideo() {
    // Check if Vulkan Video is supported
    if (!isVulkanVideoAvailable(config_.physical_device)) {
        return false;
    }

    // Create Vulkan Video decoder
    vulkan_decoder_ = std::make_unique<VulkanVideoDecoder>();

    VulkanVideoDecoderConfig vk_config;
    vk_config.max_width = config_.max_width;
    vk_config.max_height = config_.max_height;
    vk_config.dpb_slot_count = config_.dpb_slot_count;

    if (!vulkan_decoder_->initialize(config_.instance,
                                      config_.physical_device,
                                      config_.device,
                                      config_.video_decode_queue_family,
                                      config_.video_decode_queue,
                                      vk_config)) {
        MLOG_ERROR("UnifiedDec", "Failed to initialize Vulkan Video decoder");
        vulkan_decoder_.reset();
        return false;
    }

    // Create YUV converter
    yuv_converter_ = std::make_unique<VulkanYuvConverter>();

    YuvConverterConfig yuv_config;
    yuv_config.max_width = config_.max_width;
    yuv_config.max_height = config_.max_height;
    yuv_config.color_space = ColorSpace::BT709;

    if (!yuv_converter_->initialize(config_.device,
                                     config_.physical_device,
                                     config_.compute_queue_family,
                                     config_.compute_queue,
                                     yuv_config)) {
        MLOG_ERROR("UnifiedDec", "Failed to initialize YUV converter");
        vulkan_decoder_.reset();
        yuv_converter_.reset();
        return false;
    }

    // Set up frame callback chain
    vulkan_decoder_->setFrameCallback([this](VkImage nv12, VkImageView view,
                                             uint32_t w, uint32_t h, int64_t pts) {
        onVulkanFrame(nv12, view, w, h, pts);
    });

    MLOG_INFO("UnifiedDec", "Vulkan Video pipeline initialized");
    return true;
}

bool UnifiedDecoder::initializeFFmpeg() {
    ffmpeg_decoder_ = std::make_unique<FFmpegDecoder>();

    if (!ffmpeg_decoder_->init()) {
        MLOG_ERROR("UnifiedDec", "Failed to initialize FFmpeg decoder");
        ffmpeg_decoder_.reset();
        return false;
    }

    ffmpeg_decoder_->setCallback([this](const uint8_t* data, int w, int h, int64_t pts) {
        onFFmpegFrame(data, static_cast<uint32_t>(w), static_cast<uint32_t>(h), pts);
    });

    MLOG_INFO("UnifiedDec", "FFmpeg fallback decoder initialized");
    return true;
}

void UnifiedDecoder::destroy() {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    // Destroy plane views before converter
    destroyPlaneViews();

    vulkan_decoder_.reset();
    yuv_converter_.reset();
    ffmpeg_decoder_.reset();

    backend_ = DecoderBackend::Unknown;
    initialized_ = false;

    MLOG_INFO("UnifiedDec", "Decoder destroyed (decoded %llu frames, %llu errors)",
              (unsigned long long)frames_decoded_.load(),
              (unsigned long long)errors_count_.load());
}

bool UnifiedDecoder::decode(const uint8_t* nal_data, size_t nal_size, int64_t pts) {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (!initialized_ || !nal_data || nal_size == 0) {
        return false;
    }

    bool success = false;

    switch (backend_) {
        case DecoderBackend::VulkanVideo:
            if (vulkan_decoder_) {
                auto result = vulkan_decoder_->decode(nal_data, nal_size, pts);
                success = result.success;
                if (!success && !result.error_message.empty()) {
                    MLOG_ERROR("UnifiedDec", "Vulkan decode error: %s", result.error_message.c_str());
                }
            }
            break;

        case DecoderBackend::FFmpegHW:
        case DecoderBackend::FFmpegSW:
            if (ffmpeg_decoder_) {
                int frames = ffmpeg_decoder_->decode(nal_data, nal_size);
                success = (frames > 0);
            }
            break;

        default:
            break;
    }

    if (!success) {
        errors_count_++;
    }

    return success;
}

int UnifiedDecoder::decodeAccessUnit(const uint8_t* data, size_t size, int64_t pts) {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (!initialized_) return 0;

    int frames_out = 0;

    switch (backend_) {
        case DecoderBackend::VulkanVideo:
            if (vulkan_decoder_) {
                auto results = vulkan_decoder_->decodeAccessUnit(data, size, pts);
                frames_out = static_cast<int>(results.size());
            }
            break;

        case DecoderBackend::FFmpegHW:
        case DecoderBackend::FFmpegSW:
            if (ffmpeg_decoder_) {
                frames_out = ffmpeg_decoder_->decode(data, size);
            }
            break;

        default:
            break;
    }

    return frames_out;
}

int UnifiedDecoder::flush() {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (!initialized_) return 0;

    int frames_out = 0;

    switch (backend_) {
        case DecoderBackend::VulkanVideo:
            if (vulkan_decoder_) {
                auto results = vulkan_decoder_->flush();
                frames_out = static_cast<int>(results.size());
            }
            break;

        case DecoderBackend::FFmpegHW:
        case DecoderBackend::FFmpegSW:
            if (ffmpeg_decoder_) {
                frames_out = ffmpeg_decoder_->flush();
            }
            break;

        default:
            break;
    }

    return frames_out;
}

void UnifiedDecoder::setFrameCallback(DecodedFrameCallback callback) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    frame_callback_ = std::move(callback);
}

bool UnifiedDecoder::createPlaneViews(VkImage nv12_image, uint32_t width, uint32_t height) {
    // Skip if same image and dimensions
    if (nv12_image == current_nv12_image_ &&
        width == plane_view_width_ && height == plane_view_height_ &&
        y_plane_view_ != VK_NULL_HANDLE && uv_plane_view_ != VK_NULL_HANDLE) {
        return true;
    }

    // Destroy old views
    destroyPlaneViews();

    current_nv12_image_ = nv12_image;
    plane_view_width_ = width;
    plane_view_height_ = height;

    // Create Y plane view (R8_UNORM for luminance)
    VkImageViewCreateInfo y_view_info = {};
    y_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    y_view_info.image = nv12_image;
    y_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    y_view_info.format = VK_FORMAT_R8_UNORM;  // Y plane: single channel
    y_view_info.components.r = VK_COMPONENT_SWIZZLE_R;
    y_view_info.components.g = VK_COMPONENT_SWIZZLE_ZERO;
    y_view_info.components.b = VK_COMPONENT_SWIZZLE_ZERO;
    y_view_info.components.a = VK_COMPONENT_SWIZZLE_ONE;
    y_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;  // Y plane
    y_view_info.subresourceRange.baseMipLevel = 0;
    y_view_info.subresourceRange.levelCount = 1;
    y_view_info.subresourceRange.baseArrayLayer = 0;
    y_view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(config_.device, &y_view_info, nullptr, &y_plane_view_) != VK_SUCCESS) {
        MLOG_ERROR("UnifiedDec", "Failed to create Y plane view");
        return false;
    }

    // Create UV plane view (R8G8_UNORM for chrominance)
    VkImageViewCreateInfo uv_view_info = {};
    uv_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    uv_view_info.image = nv12_image;
    uv_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    uv_view_info.format = VK_FORMAT_R8G8_UNORM;  // UV plane: two channels interleaved
    uv_view_info.components.r = VK_COMPONENT_SWIZZLE_R;
    uv_view_info.components.g = VK_COMPONENT_SWIZZLE_G;
    uv_view_info.components.b = VK_COMPONENT_SWIZZLE_ZERO;
    uv_view_info.components.a = VK_COMPONENT_SWIZZLE_ONE;
    uv_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;  // UV plane
    uv_view_info.subresourceRange.baseMipLevel = 0;
    uv_view_info.subresourceRange.levelCount = 1;
    uv_view_info.subresourceRange.baseArrayLayer = 0;
    uv_view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(config_.device, &uv_view_info, nullptr, &uv_plane_view_) != VK_SUCCESS) {
        MLOG_ERROR("UnifiedDec", "Failed to create UV plane view");
        vkDestroyImageView(config_.device, y_plane_view_, nullptr);
        y_plane_view_ = VK_NULL_HANDLE;
        return false;
    }

    MLOG_INFO("UnifiedDec", "Created NV12 plane views for %dx%d", width, height);
    return true;
}

void UnifiedDecoder::destroyPlaneViews() {
    if (y_plane_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(config_.device, y_plane_view_, nullptr);
        y_plane_view_ = VK_NULL_HANDLE;
    }
    if (uv_plane_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(config_.device, uv_plane_view_, nullptr);
        uv_plane_view_ = VK_NULL_HANDLE;
    }
    current_nv12_image_ = VK_NULL_HANDLE;
}

void UnifiedDecoder::onVulkanFrame(VkImage nv12_image, VkImageView nv12_view,
                                    uint32_t width, uint32_t height, int64_t pts) {
    (void)nv12_view;  // Not used directly, we create plane-specific views

    current_width_ = width;
    current_height_ = height;

    // Convert NV12 to RGBA using compute shader
    if (!yuv_converter_ || !yuv_converter_->isInitialized()) {
        MLOG_ERROR("UnifiedDec", "YUV converter not available");
        return;
    }

    // Create/resize output image if needed
    if (!yuv_converter_->createOutputImage(width, height)) {
        MLOG_ERROR("UnifiedDec", "Failed to create output image");
        return;
    }

    // Create Y and UV plane views for NV12 image
    if (!createPlaneViews(nv12_image, width, height)) {
        MLOG_ERROR("UnifiedDec", "Failed to create NV12 plane views");
        return;
    }

    // Perform NV12 to RGBA conversion using compute shader
    if (!yuv_converter_->convert(nv12_image, y_plane_view_, uv_plane_view_,
                                  width, height,
                                  yuv_converter_->outputImage(),
                                  yuv_converter_->outputView())) {
        MLOG_ERROR("UnifiedDec", "YUV conversion failed");
        return;
    }

    // Build frame and call user callback
    DecodedFrameCallback callback_copy;
    {
        std::lock_guard<std::mutex> lock(decode_mutex_);
        callback_copy = frame_callback_;
    }
    if (callback_copy) {
        DecodedFrame frame;
        frame.vk_image = yuv_converter_->outputImage();
        frame.vk_view = yuv_converter_->outputView();
        frame.vk_descriptor = yuv_converter_->outputDescriptorSet();
        frame.width = width;
        frame.height = height;
        frame.pts = pts;
        frame.backend = DecoderBackend::VulkanVideo;

        callback_copy(frame);
    }

    frames_decoded_++;

    if (frames_decoded_ <= 5 || frames_decoded_ % 100 == 0) {
        MLOG_INFO("UnifiedDec", "Vulkan frame #%llu: %dx%d",
                  (unsigned long long)frames_decoded_, width, height);
    }
}

void UnifiedDecoder::onFFmpegFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height, int64_t pts) {
    current_width_ = width;
    current_height_ = height;

    DecodedFrameCallback callback_copy;
    {
        std::lock_guard<std::mutex> lock(decode_mutex_);
        callback_copy = frame_callback_;
    }
    if (callback_copy) {
        DecodedFrame frame;
        frame.rgba_data = rgba_data;
        frame.owns_data = false;  // Data owned by FFmpeg decoder
        frame.width = width;
        frame.height = height;
        frame.pts = pts;
        frame.backend = backend_;

        callback_copy(frame);
    }

    frames_decoded_++;

    if (frames_decoded_ <= 5 || frames_decoded_ % 100 == 0) {
        MLOG_INFO("UnifiedDec", "FFmpeg frame #%llu: %dx%d",
                  (unsigned long long)frames_decoded_, width, height);
    }
}

const char* UnifiedDecoder::backendName() const {
    switch (backend_) {
        case DecoderBackend::VulkanVideo: return "Vulkan Video (Tier 1)";
        case DecoderBackend::FFmpegHW:    return "FFmpeg D3D11VA (Tier 2)";
        case DecoderBackend::FFmpegSW:    return "FFmpeg CPU (Tier 3)";
        default:                          return "Unknown";
    }
}

bool UnifiedDecoder::isVulkanVideoAvailable(VkPhysicalDevice physical_device) {
    if (physical_device == VK_NULL_HANDLE) {
        return false;
    }

    return VulkanVideoDecoder::isSupported(physical_device);
}

} // namespace mirage::video
