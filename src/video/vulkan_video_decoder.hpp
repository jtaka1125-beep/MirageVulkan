// =============================================================================
// MirageSystem - Vulkan Video H.264 Decoder
// =============================================================================
// Zero-copy H.264 decode pipeline using VK_KHR_video_decode_h264
// Output: NV12 VkImage (GPU-resident, no CPU readback)
// =============================================================================
#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <array>
#include <memory>
#include <functional>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <string>

#include "h264_parser.hpp"  // H264SPS, H264PPS, H264SliceHeader

namespace mirage::video {

// Forward declarations
struct H264SPS;
struct H264PPS;
struct H264SliceHeader;

// =============================================================================
// NAL Unit Types
// =============================================================================
enum class NalUnitType : uint8_t {
    UNSPECIFIED = 0,
    SLICE_NON_IDR = 1,
    SLICE_PART_A = 2,
    SLICE_PART_B = 3,
    SLICE_PART_C = 4,
    SLICE_IDR = 5,
    SEI = 6,
    SPS = 7,
    PPS = 8,
    AUD = 9,
    END_SEQUENCE = 10,
    END_STREAM = 11,
    FILLER = 12,
    SPS_EXT = 13,
    PREFIX = 14,
    SUBSET_SPS = 15,
    RESERVED_16 = 16,
    RESERVED_17 = 17,
    RESERVED_18 = 18,
    AUX_SLICE = 19,
    SLICE_EXT = 20,
    SLICE_EXT_DEPTH = 21,
};

// =============================================================================
// Decoded Picture Buffer (DPB) Slot
// =============================================================================
struct DpbSlot {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    int32_t frame_num = -1;
    int32_t poc = -1;           // Picture Order Count
    bool is_reference = false;
    bool is_long_term = false;
    bool in_use = false;

    void reset() {
        frame_num = -1;
        poc = -1;
        is_reference = false;
        is_long_term = false;
        in_use = false;
    }
};

// =============================================================================
// Vulkan Video Function Pointers (per-instance for multi-device support)
// =============================================================================
struct VulkanVideoFunctions {
    PFN_vkCreateVideoSessionKHR CreateVideoSession = nullptr;
    PFN_vkDestroyVideoSessionKHR DestroyVideoSession = nullptr;
    PFN_vkGetVideoSessionMemoryRequirementsKHR GetVideoSessionMemoryRequirements = nullptr;
    PFN_vkBindVideoSessionMemoryKHR BindVideoSessionMemory = nullptr;
    PFN_vkCreateVideoSessionParametersKHR CreateVideoSessionParameters = nullptr;
    PFN_vkUpdateVideoSessionParametersKHR UpdateVideoSessionParameters = nullptr;
    PFN_vkDestroyVideoSessionParametersKHR DestroyVideoSessionParameters = nullptr;
    PFN_vkCmdBeginVideoCodingKHR CmdBeginVideoCoding = nullptr;
    PFN_vkCmdEndVideoCodingKHR CmdEndVideoCoding = nullptr;
    PFN_vkCmdControlVideoCodingKHR CmdControlVideoCoding = nullptr;
    PFN_vkCmdDecodeVideoKHR CmdDecodeVideo = nullptr;

    bool isLoaded() const { return CreateVideoSession != nullptr; }
};

// =============================================================================
// Vulkan Video Capabilities (queried at runtime)
// =============================================================================
struct VulkanVideoCapabilities {
    uint32_t max_width = 0;
    uint32_t max_height = 0;
    uint32_t min_width = 0;
    uint32_t min_height = 0;
    uint32_t max_dpb_slots = 0;
    uint32_t max_active_reference_pictures = 0;
    bool supports_h264_decode = false;
    bool supports_h265_decode = false;
    uint8_t max_level_idc = 0;  // H.264 max level (e.g., 51 = 5.1)

    // Bitstream buffer alignment requirements
    VkDeviceSize minBitstreamBufferOffsetAlignment = 1;
    VkDeviceSize minBitstreamBufferSizeAlignment = 1;

    // Header version from capabilities query (for pStdHeaderVersion)
    VkExtensionProperties stdHeaderVersion = {};
    bool hasStdHeaderVersion = false;
};

// =============================================================================
// Vulkan Video Decoder Configuration
// =============================================================================
struct VulkanVideoDecoderConfig {
    uint32_t max_width = 1920;
    uint32_t max_height = 1080;
    uint32_t dpb_slot_count = 8;    // Decoded Picture Buffer slots
    bool enable_reference_pictures = true;
    bool async_decode = true;       // Enable async decode (frame-in-flight)
};

// =============================================================================
// Decode Result
// =============================================================================
struct DecodeResult {
    bool success = false;
    VkImage output_image = VK_NULL_HANDLE;      // NV12 decoded frame
    VkImageView output_view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t pts = 0;
    int32_t poc = 0;

    std::string error_message;
};

// =============================================================================
// Frame callback (NV12 VkImage ready for YUV→RGBA conversion)
// =============================================================================
using FrameCallback = std::function<void(VkImage nv12_image, VkImageView nv12_view,
                                         uint32_t width, uint32_t height, int64_t pts)>;

// =============================================================================
// VulkanVideoDecoder - H.264 decoder using Vulkan Video
// =============================================================================
class VulkanVideoDecoder {
public:
    VulkanVideoDecoder();
    ~VulkanVideoDecoder();

    // Non-copyable, non-movable
    VulkanVideoDecoder(const VulkanVideoDecoder&) = delete;
    VulkanVideoDecoder& operator=(const VulkanVideoDecoder&) = delete;

    // Initialize with Vulkan device
    bool initialize(VkInstance instance,
                    VkPhysicalDevice physical_device,
                    VkDevice device,
                    uint32_t video_decode_queue_family,
                    VkQueue video_decode_queue,
                    const VulkanVideoDecoderConfig& config = {});

    // Check if Vulkan Video H.264 decode is supported
    static bool isSupported(VkPhysicalDevice physical_device);

    // Query video decode capabilities (requires VkInstance for proper function loading)
    static bool queryCapabilities(VkInstance instance,
                                  VkPhysicalDevice physical_device,
                                  VulkanVideoCapabilities& caps);

    // Get current capabilities (after initialization)
    const VulkanVideoCapabilities& capabilities() const { return capabilities_; }

    // Decode H.264 NAL unit (Annex-B format with start codes)
    DecodeResult decode(const uint8_t* nal_data, size_t nal_size, int64_t pts = 0);

    // Decode complete access unit (may contain multiple NALs)
    std::vector<DecodeResult> decodeAccessUnit(const uint8_t* data, size_t size, int64_t pts = 0);

    // Flush decoder (output all buffered frames)
    std::vector<DecodeResult> flush();

    // Set frame callback (thread-safe)
    void setFrameCallback(FrameCallback callback) {
        std::lock_guard<std::mutex> lock(decode_mutex_);
        frame_callback_ = std::move(callback);
    }

    // Get current resolution
    uint32_t width() const { return current_width_; }
    uint32_t height() const { return current_height_; }

    // Statistics
    uint64_t framesDecoded() const { return frames_decoded_; }
    uint64_t errorsCount() const { return errors_count_; }

    // Check initialization state
    bool isInitialized() const { return initialized_; }

    // Cleanup
    void destroy();

private:
    // NAL parsing
    bool parseNalUnit(const uint8_t* data, size_t size);
    bool parseSPS(const uint8_t* data, size_t size);
    bool parsePPS(const uint8_t* data, size_t size);
    bool parseSliceHeader(const uint8_t* data, size_t size, H264SliceHeader& header);

    // Video session management
    bool createVideoSession();
    bool createVideoSessionParameters();
    void destroyVideoSession();

    // DPB management
    bool allocateDpbSlots();
    void freeDpbSlots();
    int32_t acquireDpbSlot();
    void releaseDpbSlot(int32_t index);
    int32_t findDpbSlotByPoc(int32_t poc);

    // Frame-in-flight bitstream buffers
    bool createFrameBitstreamBuffer(uint32_t frame_index, size_t size);
    void destroyFrameBitstreamBuffers();
    uint32_t acquireFrameResources();  // Wait for and acquire next frame slot
    void releaseFrameResources(uint32_t frame_index);  // Mark frame as complete

    // Decode operations
    DecodeResult decodeSlice(const uint8_t* nal_data, size_t nal_size, int64_t pts);

    // POC calculation (supports pic_order_cnt_type 0/1/2)
    int32_t calculatePOC(const H264SliceHeader& header, const H264SPS& sps, bool is_idr, uint8_t nal_ref_idc);

    // Long-term reference management (MMCO operations)
    void applyRefPicMarking(const H264SliceHeader& header, bool is_idr, int32_t current_slot);

private:
    // Vulkan Video function pointers (per-instance)
    VulkanVideoFunctions vkfn_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue video_queue_ = VK_NULL_HANDLE;
    uint32_t video_queue_family_ = 0;

    // Video session
    VkVideoSessionKHR video_session_ = VK_NULL_HANDLE;
    VkVideoSessionParametersKHR session_params_ = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> session_memory_;

    // Command pool for decode operations
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;

    // Frame-in-flight resources for async decode
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

    struct FrameResources {
        VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
        VkBuffer bitstream_buffer = VK_NULL_HANDLE;
        VkDeviceMemory bitstream_memory = VK_NULL_HANDLE;
        size_t bitstream_buffer_size = 0;
        void* bitstream_mapped = nullptr;
        uint64_t timeline_value = 0;  // Timeline semaphore value for this frame
        bool in_use = false;
    };
    std::array<FrameResources, MAX_FRAMES_IN_FLIGHT> frame_resources_;
    uint32_t current_frame_index_ = 0;

    // Timeline semaphore for async decode
    VkSemaphore timeline_semaphore_ = VK_NULL_HANDLE;
    uint64_t timeline_value_ = 0;

    // Decoded Picture Buffer
    std::vector<DpbSlot> dpb_slots_;
    int32_t active_sps_id_ = -1;
    int32_t active_pps_id_ = -1;

    // Parsed parameter sets
    std::vector<std::unique_ptr<H264SPS>> sps_list_;
    std::vector<std::unique_ptr<H264PPS>> pps_list_;

    // Current decode state
    uint32_t current_width_ = 0;
    uint32_t current_height_ = 0;
    int32_t prev_frame_num_ = 0;
    int32_t prev_poc_ = 0;
    bool first_slice_ = true;

    // POC calculation state (for pic_order_cnt_type 0)
    int32_t prev_poc_msb_ = 0;
    int32_t prev_poc_lsb_ = 0;

    // POC calculation state (for pic_order_cnt_type 1/2)
    int32_t frame_num_offset_ = 0;
    int32_t prev_frame_num_offset_ = 0;

    // MMCO state (for adaptive ref pic marking)
    int32_t max_long_term_frame_idx_ = -1;

    // B-frame reorder buffer (for display order output)
    struct PendingFrame {
        int32_t dpb_slot;
        int32_t poc;
        int64_t pts;
        bool output_ready;
    };
    std::vector<PendingFrame> reorder_buffer_;
    int32_t last_output_poc_ = INT32_MIN;
    static constexpr size_t MAX_REORDER_BUFFER = 16;

    // Helper for reorder output
    void outputReorderedFrames(bool flush_all = false);

    // Configuration
    VulkanVideoDecoderConfig config_;

    // Queried capabilities
    VulkanVideoCapabilities capabilities_;

    // Callback
    FrameCallback frame_callback_;

    // Statistics
    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> errors_count_{0};

    // Thread safety
    std::mutex decode_mutex_;

    // Initialization state
    bool initialized_ = false;
};

// H264SPS, H264PPS, H264SliceHeader are now defined in h264_parser.hpp

} // namespace mirage::video
