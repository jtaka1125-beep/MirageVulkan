#pragma once
#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <vulkan/vulkan.h>

namespace gui {

// Forward declaration (legacy FFmpeg decoder)
class H264Decoder;
}

namespace mirage::video {
class UnifiedDecoder;
}

namespace gui {

/**
 * Mirror video frame (decoded RGBA)
 */
struct MirrorFrame {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
  uint64_t pts_us = 0;
  uint64_t frame_id = 0;
};

/**
 * UDP Mirror Receiver
 * - Receives RTP H.264 packets on specified port
 * - Depacketizes and decodes via FFmpeg (or test pattern if USE_FFMPEG not defined)
 * - Provides latest frame for display
 */
class MirrorReceiver {
public:
  // FU-A buffer limits (prevent DoS via malformed packets)
  static constexpr size_t MAX_FU_BUFFER_SIZE = 2 * 1024 * 1024;  // 2MB max NAL size
  static constexpr size_t MAX_SPS_SIZE = 256;   // SPS should be small
  static constexpr size_t MAX_PPS_SIZE = 256;   // PPS should be small

  MirrorReceiver();
  ~MirrorReceiver();

  // Configure Vulkan context for UnifiedDecoder (must call before start)
  void setVulkanContext(VkPhysicalDevice physical_device, VkDevice device,
                        uint32_t graphics_queue_family, VkQueue graphics_queue,
                        uint32_t compute_queue_family, VkQueue compute_queue,
                        uint32_t video_decode_queue_family = UINT32_MAX,
                        VkQueue video_decode_queue = VK_NULL_HANDLE);

  // Start receiving on port (default 5000)
  bool start(uint16_t port = 5000);
  void stop();

  // Initialize decoder only (no UDP socket) - for external data feed
  bool init_decoder();

  // Start decoder + decode thread only (no UDP socket) - for TCP receiver mode
  bool start_decoder_only();

  // Start TCP receive mode (connects to localhost:tcp_port for raw H.264)
  bool start_tcp(uint16_t tcp_port);

  bool running() const { return running_.load(); }

  // Get assigned port (valid after start(), returns 0 if not started)
  uint16_t getPort(int timeout_ms = 2000) const {
    auto start = std::chrono::steady_clock::now();
    while (bound_port_.load() == 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count();
      if (elapsed > timeout_ms) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return bound_port_.load();
  }

  // Get latest frame (thread-safe, returns false if no new frame)
  bool get_latest_frame(MirrorFrame& out);

  // Stats
  uint64_t packets_received() const { return packets_received_.load(); }
  uint64_t nals_received() const { return nals_received_.load(); }
  uint64_t frames_decoded() const { return frames_decoded_.load(); }
  uint64_t bytes_received() const { return bytes_received_.load(); }

  // Feed RTP packet from external source (e.g., USB AOA)
  void feed_rtp_packet(const uint8_t* data, size_t len);

private:
  void receive_thread(uint16_t port);
  void tcp_receive_thread(uint16_t tcp_port);
  void process_rtp_packet(const uint8_t* data, size_t len);
  void process_raw_h264(const uint8_t* data, size_t len);
  size_t find_start_code(const uint8_t* data, size_t len, size_t offset);
  void decode_nal(const uint8_t* data, size_t len);
  void generate_test_frame(int w, int h);

  std::atomic<bool> running_{false};
  std::atomic<uint16_t> bound_port_{0};
  uint16_t tcp_port_{0};  // TCP port for scrcpy direct connection  // Actual port after bind (0 = auto-assign)
  std::thread thread_;

  // Frame buffer (double buffered)
  std::mutex frame_mtx_;
  MirrorFrame current_frame_;
  bool has_new_frame_ = false;

  // RTP depacketizer state
  bool have_fu_ = false;
  uint16_t fu_start_seq_ = 0;
  std::atomic<uint16_t> last_seq_{0};
  std::vector<uint8_t> fu_buf_;

  // Stats
  std::atomic<uint64_t> packets_received_{0};
  std::atomic<uint64_t> nals_received_{0};
  std::atomic<uint64_t> frames_decoded_{0};
  std::atomic<uint64_t> bytes_received_{0};

  // Decode thread (separated from receive for pipeline parallelism)
  std::thread decode_thread_;
  void decode_thread_func();
  
  // NAL queue (receive -> decode)
  struct NalUnit {
    std::vector<uint8_t> data;
  };
  std::queue<NalUnit> nal_queue_;
  std::mutex nal_queue_mtx_;
  std::condition_variable nal_queue_cv_;
  static constexpr size_t MAX_NAL_QUEUE_SIZE = 128;
  
  // Enqueue NAL for async decode
  void enqueue_nal(const uint8_t* data, size_t len);

  // Test pattern state
  uint64_t test_frame_id_ = 0;

  // Unified decoder (Vulkan Video / FFmpeg fallback)
  std::unique_ptr<mirage::video::UnifiedDecoder> unified_decoder_;
  void on_unified_frame(const uint8_t* rgba, int width, int height, int64_t pts);

  // Vulkan context for UnifiedDecoder
  VkPhysicalDevice vk_physical_device_ = VK_NULL_HANDLE;
  VkDevice vk_device_ = VK_NULL_HANDLE;
  uint32_t vk_graphics_queue_family_ = 0;
  VkQueue vk_graphics_queue_ = VK_NULL_HANDLE;
  uint32_t vk_compute_queue_family_ = 0;
  VkQueue vk_compute_queue_ = VK_NULL_HANDLE;
  uint32_t vk_video_decode_queue_family_ = UINT32_MAX;
  VkQueue vk_video_decode_queue_ = VK_NULL_HANDLE;
  bool use_unified_decoder_ = false;

  // Legacy FFmpeg decoder (fallback if UnifiedDecoder not available)
#ifdef USE_FFMPEG
  std::unique_ptr<H264Decoder> decoder_;
  void on_decoded_frame(const uint8_t* rgba, int width, int height, uint64_t pts);
#endif

  // SPS/PPS cache for stream recovery
  std::vector<uint8_t> cached_sps_;
  std::vector<uint8_t> cached_pps_;
  bool sps_logged_ = false;
  bool pps_logged_ = false;

  // Raw H.264 Annex B accumulation buffer (for scrcpy raw_stream=true)
  std::vector<uint8_t> raw_h264_buf_;

  // Reusable annexb buffer for decode_nal (avoids per-NAL heap allocation)
  std::vector<uint8_t> annexb_buf_;
};

} // namespace gui
