#pragma once
#include <cstdint>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>

// Forward declarations for FFmpeg types
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct SwsContext;

namespace gui {

/**
 * H.264 Decoder using FFmpeg.
 * - Input: AnnexB NAL units (with 00 00 00 01 start codes)
 * - Output: RGBA frames via callback
 *
 * Thread Safety:
 * - The decoder itself is NOT thread-safe
 * - Callbacks are invoked synchronously during decode()
 * - Callback receives a pointer to an internal buffer that is valid
 *   ONLY for the duration of the callback. Copy data if needed.
 *
 * Memory:
 * - RGBA buffer is managed internally
 * - Callback must NOT store the raw pointer; copy data instead
 */
class H264Decoder {
public:
  /**
   * Callback for decoded frames.
   * WARNING: The rgba pointer is only valid during the callback invocation.
   *          Copy the data if you need it after the callback returns.
   *
   * @param rgba   Pointer to RGBA data (width * height * 4 bytes)
   * @param width  Frame width in pixels
   * @param height Frame height in pixels
   * @param pts    Presentation timestamp
   */
  using FrameCallback = std::function<void(const uint8_t* rgba, int width, int height, uint64_t pts)>;

  H264Decoder();
  ~H264Decoder();

  // Non-copyable, non-movable (owns FFmpeg resources)
  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;
  H264Decoder(H264Decoder&&) = delete;
  H264Decoder& operator=(H264Decoder&&) = delete;

  // Initialize decoder
  bool init(bool use_hevc = false);

  // Set callback for decoded frames
  void set_frame_callback(FrameCallback cb) { frame_callback_ = cb; }

  // Feed AnnexB NAL data (with start codes)
  // Returns number of frames decoded
  int decode(const uint8_t* annexb_data, size_t len);

  // Flush decoder (get remaining frames)
  int flush();

  // Stats
  uint64_t nals_fed() const { return nals_fed_; }
  uint64_t frames_decoded() const { return frames_decoded_; }
  uint64_t error_count() const { return error_count_; }

  bool is_initialized() const { return codec_ctx_ != nullptr; }

private:
  int decode_packet(AVPacket* pkt);
  void convert_frame_to_rgba(AVFrame* frame);

  AVCodecContext* codec_ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVFrame* frame_rgba_ = nullptr;
  AVPacket* packet_ = nullptr;
  SwsContext* sws_ctx_ = nullptr;

  int last_width_ = 0;
  int out_width_ = 0;
  int out_height_ = 0;
  int last_height_ = 0;

  FrameCallback frame_callback_;

  // Persistent RGBA buffer to avoid repeated allocations
  std::vector<uint8_t> rgba_buffer_;

  uint64_t nals_fed_ = 0;
  uint64_t frames_decoded_ = 0;

  // Error counters (per-instance, not static)
  uint64_t error_count_ = 0;

  // HW acceleration
  bool hw_enabled_ = false;
  int hw_pix_fmt_ = -1;  // AV_PIX_FMT_D3D11 or AV_PIX_FMT_VULKAN (or -1 if CPU)
  AVBufferRef* hw_device_ctx_ = nullptr;
  AVFrame* sw_frame_ = nullptr;  // Pre-allocated frame for HW->CPU transfer
  bool is_hevc_ = false;
  uint64_t send_packet_errors_ = 0;
  uint64_t receive_frame_errors_ = 0;

  // Global instance counter: only first instance uses D3D11VA.
  // Prevents multi-instance GPU scheduler contention/stalls.
  static std::atomic<int> s_instance_count_;
  int instance_index_ = 0;
};

} // namespace gui
