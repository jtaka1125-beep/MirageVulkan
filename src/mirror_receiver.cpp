#include "mirror_receiver.hpp"
#include "video/unified_decoder.hpp"

#ifdef USE_FFMPEG
#include "video/h264_decoder.hpp"
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#include <cstring>
#include <cstdio>
#include <system_error>
#include <atomic>
#include <mutex>
#include "mirage_log.hpp"
#include "vid0_parser.hpp"

namespace gui {
// ==============================================================================
// BitReader — H.264 NALユニットのビットストリーム読み取りヘルパー
// ==============================================================================
class BitReader {
public:
  BitReader(const uint8_t* data, size_t len)
    : data_(data), len_(len), byte_pos_(0), bit_pos_(0) {}

  // nビット読み取り（最大32ビット）
  uint32_t read_bits(int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
      if (byte_pos_ >= len_) return val;  // ストリーム終端
      val = (val << 1) | ((data_[byte_pos_] >> (7 - bit_pos_)) & 1);
      if (++bit_pos_ == 8) {
        bit_pos_ = 0;
        byte_pos_++;
      }
    }
    return val;
  }

  // Exp-Golomb符号読み取り（unsigned）
  uint32_t read_exp_golomb() {
    int leading_zeros = 0;
    while (byte_pos_ < len_ && read_bits(1) == 0) {
      leading_zeros++;
      if (leading_zeros > 31) return 0;  // 異常値防止
    }
    if (leading_zeros == 0) return 0;
    uint32_t val = read_bits(leading_zeros);
    return (1u << leading_zeros) - 1 + val;
  }

  bool has_data() const { return byte_pos_ < len_; }

private:
  const uint8_t* data_;
  size_t len_;
  size_t byte_pos_;
  int bit_pos_;
};

// ==============================================================================
// SPS次元パーサ — 解像度を抽出してバリデーション
// ==============================================================================
bool MirrorReceiver::parse_sps_dimensions(const uint8_t* sps_data, size_t sps_len,
                                           int& width, int& height) {
  // SPS NAL payload requires at least a few bytes
  if (sps_len < 4) return false;

  // Convert EBSP -> RBSP by removing emulation_prevention_three_byte (0x03)
  // This is required for correct bit parsing.
  std::vector<uint8_t> rbsp;
  rbsp.reserve(sps_len);
  int zero_count = 0;
  for (size_t i = 1; i < sps_len; ++i) {
    const uint8_t b = sps_data[i];
    if (zero_count == 2 && b == 0x03) {
      zero_count = 0;
      continue;
    }
    rbsp.push_back(b);
    if (b == 0x00) zero_count++;
    else zero_count = 0;
  }
  if (rbsp.size() < 3) return false;

  BitReader br(rbsp.data(), rbsp.size());

  uint32_t profile_idc = br.read_bits(8);
  br.read_bits(8);  // constraint_set flags + reserved
  br.read_bits(8);  // level_idc

  br.read_exp_golomb();  // seq_parameter_set_id

  // Defaults for non-high profiles
  uint32_t chroma_format_idc = 1;  // 4:2:0
  uint32_t separate_colour_plane_flag = 0;

  // High profile extras
  if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
      profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
      profile_idc == 86  || profile_idc == 118 || profile_idc == 128 ||
      profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {
    chroma_format_idc = br.read_exp_golomb();
    if (chroma_format_idc == 3) {
      separate_colour_plane_flag = br.read_bits(1);
    }
    br.read_exp_golomb();  // bit_depth_luma_minus8
    br.read_exp_golomb();  // bit_depth_chroma_minus8
    br.read_bits(1);       // qpprime_y_zero_transform_bypass_flag
    uint32_t seq_scaling_matrix_present = br.read_bits(1);
    if (seq_scaling_matrix_present) {
      int cnt = (chroma_format_idc != 3) ? 8 : 12;
      for (int i = 0; i < cnt; i++) {
        uint32_t present = br.read_bits(1);
        if (present) {
          int size = (i < 6) ? 16 : 64;
          int last_scale = 8, next_scale = 8;
          for (int j = 0; j < size; j++) {
            if (next_scale != 0) {
              int delta = static_cast<int>(br.read_exp_golomb());
              // Map UE to SE (H.264 signed Exp-Golomb)
              delta = (delta & 1) ? ((delta + 1) >> 1) : -(delta >> 1);
              next_scale = (last_scale + delta + 256) % 256;
            }
            last_scale = (next_scale == 0) ? last_scale : next_scale;
          }
        }
      }
    }
  }

  br.read_exp_golomb();  // log2_max_frame_num_minus4

  uint32_t pic_order_cnt_type = br.read_exp_golomb();
  if (pic_order_cnt_type == 0) {
    br.read_exp_golomb();
  } else if (pic_order_cnt_type == 1) {
    br.read_bits(1);
    br.read_exp_golomb();
    br.read_exp_golomb();
    uint32_t num_ref = br.read_exp_golomb();
    for (uint32_t i = 0; i < num_ref && i < 256; i++) {
      br.read_exp_golomb();
    }
  }

  br.read_exp_golomb();  // max_num_ref_frames
  br.read_bits(1);       // gaps_in_frame_num_value_allowed_flag

  uint32_t pic_width_in_mbs_minus1 = br.read_exp_golomb();
  uint32_t pic_height_in_map_units_minus1 = br.read_exp_golomb();

  uint32_t frame_mbs_only_flag = br.read_bits(1);
  if (!frame_mbs_only_flag) {
    br.read_bits(1);
  }

  br.read_bits(1);  // direct_8x8_inference_flag

  uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
  uint32_t frame_cropping_flag = br.read_bits(1);
  if (frame_cropping_flag) {
    crop_left   = br.read_exp_golomb();
    crop_right  = br.read_exp_golomb();
    crop_top    = br.read_exp_golomb();
    crop_bottom = br.read_exp_golomb();
  }

  // Compute dimensions with correct cropping units
  const uint32_t chroma_array_type = (chroma_format_idc == 3 && separate_colour_plane_flag) ? 0 : chroma_format_idc;
  uint32_t crop_unit_x = 1;
  uint32_t crop_unit_y = (2 - frame_mbs_only_flag);
  if (chroma_array_type != 0) {
    uint32_t sub_width_c = 1;
    uint32_t sub_height_c = 1;
    if (chroma_format_idc == 1) { sub_width_c = 2; sub_height_c = 2; }
    else if (chroma_format_idc == 2) { sub_width_c = 2; sub_height_c = 1; }
    else if (chroma_format_idc == 3) { sub_width_c = 1; sub_height_c = 1; }
    crop_unit_x = sub_width_c;
    crop_unit_y = sub_height_c * (2 - frame_mbs_only_flag);
  }

  const int full_w = static_cast<int>((pic_width_in_mbs_minus1 + 1) * 16);
  const int full_h = static_cast<int>((pic_height_in_map_units_minus1 + 1) * (2 - frame_mbs_only_flag) * 16);

  width  = full_w - static_cast<int>((crop_left + crop_right) * crop_unit_x);
  height = full_h - static_cast<int>((crop_top + crop_bottom) * crop_unit_y);

  return (width > 0 && height > 0);
}

// Global WSA reference counting for multiple MirrorReceiver instances
#ifdef _WIN32
static std::atomic<int> g_wsa_ref_count{0};
static std::mutex g_wsa_mutex;

static bool wsa_init() {
    std::lock_guard<std::mutex> lock(g_wsa_mutex);
    if (g_wsa_ref_count.fetch_add(1) == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            g_wsa_ref_count.fetch_sub(1);
            return false;
        }
    }
    return true;
}

static void wsa_cleanup() {
    std::lock_guard<std::mutex> lock(g_wsa_mutex);
    if (g_wsa_ref_count.fetch_sub(1) == 1) {
        WSACleanup();
    }
}
#endif

MirrorReceiver::MirrorReceiver() = default;

MirrorReceiver::~MirrorReceiver() {
  stop();
}

void MirrorReceiver::setVulkanContext(VkPhysicalDevice physical_device, VkDevice device,
                                       uint32_t graphics_queue_family, VkQueue graphics_queue,
                                       uint32_t compute_queue_family, VkQueue compute_queue,
                                       uint32_t video_decode_queue_family, VkQueue video_decode_queue) {
  vk_physical_device_ = physical_device;
  vk_device_ = device;
  vk_graphics_queue_family_ = graphics_queue_family;
  vk_graphics_queue_ = graphics_queue;
  vk_compute_queue_family_ = compute_queue_family;
  vk_compute_queue_ = compute_queue;
  vk_video_decode_queue_family_ = video_decode_queue_family;
  vk_video_decode_queue_ = video_decode_queue;
}

bool MirrorReceiver::init_decoder() {
  // Try UnifiedDecoder first (Vulkan Video with FFmpeg fallback).
  // For HEVC streams, always use UnifiedDecoder because legacy H264Decoder
  // cannot handle HEVC. For H.264, prefer UnifiedDecoder when Vulkan is available.
  if (vk_device_ != VK_NULL_HANDLE || stream_is_hevc_) {
    unified_decoder_ = std::make_unique<mirage::video::UnifiedDecoder>();

    mirage::video::UnifiedDecoderConfig config;
    config.codec = stream_is_hevc_ ? mirage::video::VideoCodec::HEVC : mirage::video::VideoCodec::H264;
    config.physical_device = vk_physical_device_;
    config.device = vk_device_;
    config.graphics_queue_family = vk_graphics_queue_family_;
    config.graphics_queue = vk_graphics_queue_;
    config.compute_queue_family = vk_compute_queue_family_;
    config.compute_queue = vk_compute_queue_;
    config.video_decode_queue_family = vk_video_decode_queue_family_;
    config.video_decode_queue = vk_video_decode_queue_;
    config.prefer_vulkan_video = true;
    config.allow_ffmpeg_fallback = true;
    config.enable_hw_accel = true;
    config.max_width = 4096;
    config.max_height = 4096;

    if (unified_decoder_->initialize(config)) {
      unified_decoder_->setFrameCallback([this](const mirage::video::DecodedFrame& frame) {
        if (frame.rgba_data) {
          on_unified_frame(frame.rgba_data, frame.width, frame.height, frame.pts);
        }
      });
      use_unified_decoder_ = true;
      MLOG_INFO("mirror", "Using UnifiedDecoder: %s", unified_decoder_->backendName());
      return true;
    }

    MLOG_WARN("mirror", "UnifiedDecoder init failed, falling back to legacy decoder");
    unified_decoder_.reset();
  }

  // Fall back to legacy H264Decoder
#ifdef USE_FFMPEG
  if (decoder_) return true;  // Already initialized

  decoder_ = std::make_unique<H264Decoder>();
  if (!decoder_->init(false)) {
    decoder_.reset();
    return false;
  }
  decoder_->set_frame_callback([this](const uint8_t* rgba, int w, int h, uint64_t pts) {
    on_decoded_frame(rgba, w, h, pts);
  });
  MLOG_INFO("mirror", "Using legacy H264Decoder");
  return true;
#else
  return true;  // No decoder needed
#endif
}

bool MirrorReceiver::start_decoder_only() {
  if (running_.load()) return true;
  if (!init_decoder()) return false;
  running_.store(true);
  try {
    decode_thread_ = std::thread(&MirrorReceiver::decode_thread_func, this);
  } catch (const std::system_error& e) {
    MLOG_ERROR("mirror", "Failed to start decode thread: %s", e.what());
    running_.store(false);
    return false;
  }
  return true;
}

bool MirrorReceiver::start(uint16_t port) {
  if (running_.load()) return true;

#ifdef _WIN32
  if (!wsa_init()) {
    return false;
  }
#endif

  // Initialize decoder
  init_decoder();

  running_.store(true);

  // Start receive thread with exception safety
  try {
    thread_ = std::thread(&MirrorReceiver::receive_thread, this, port);
    decode_thread_ = std::thread(&MirrorReceiver::decode_thread_func, this);
  } catch (const std::system_error& e) {
    MLOG_ERROR("mirror", "Failed to start thread: %s", e.what());
    running_.store(false);
#ifdef _WIN32
    wsa_cleanup();
#endif
    return false;
  }

  return true;
}

void MirrorReceiver::stop() {
  running_.store(false);
  // Wake up decode thread
  nal_queue_cv_.notify_all();

  if (thread_.joinable()) {
    thread_.join();
  }
  if (decode_thread_.joinable()) {
    decode_thread_.join();
  }

  // Cleanup UnifiedDecoder
  if (unified_decoder_) {
    unified_decoder_->flush();
    unified_decoder_->destroy();
    unified_decoder_.reset();
  }
  use_unified_decoder_ = false;

#ifdef USE_FFMPEG
  if (decoder_) {
    decoder_->flush();
    decoder_.reset();
  }
#endif

#ifdef _WIN32
  wsa_cleanup();
#endif
}

bool MirrorReceiver::get_latest_frame(MirrorFrame& out) {
  std::lock_guard<std::mutex> lock(frame_mtx_);
  if (!has_new_frame_) return false;
  out.width    = cur_width_;
  out.height   = cur_height_;
  out.frame_id = cur_frame_id_;
  out.pts_us   = cur_pts_us_;
  // SharedFrame path only (current_frame_.rgba no longer populated)
  has_new_frame_ = false;
  return true;
}

// SharedFrame version: O(1) shared_ptr copy, no rgba memcpy
bool MirrorReceiver::get_latest_shared_frame(std::shared_ptr<mirage::SharedFrame>& out) {
  // A-1: Prefer ring buffer path
  if (ring_buffer_) {
    gui::FrameMetrics metrics;
    auto frame = ring_buffer_->pop_latest(&metrics);
    if (frame) {
      out = frame;
      // A-1 Observability: periodic stats log (every 10 seconds)
      static auto last_stats_log = std::chrono::steady_clock::now();
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_stats_log).count();
      if (elapsed >= 10) {
        last_stats_log = now;
        int64_t latency_us = (metrics.enqueue_ts > 0 && metrics.decode_ts > 0)
            ? (metrics.enqueue_ts - metrics.decode_ts) : 0;
        MLOG_INFO("ringbuf", "STATS: pushed=%llu popped=%llu dropped=%llu buf=%zu latency_d2e=%lld us",
            (unsigned long long)ring_buffer_->total_pushed(),
            (unsigned long long)ring_buffer_->total_popped(),
            (unsigned long long)ring_buffer_->dropped_frames(),
            ring_buffer_->current_count(),
            (long long)latency_us);
      }
      return true;
    }
    return false;
  }
  // Legacy fallback
  std::lock_guard<std::mutex> lock(frame_mtx_);
  if (!has_new_frame_ || !current_shared_frame_) return false;
  out = current_shared_frame_;  // O(1)
  has_new_frame_ = false;
  return true;
}

void MirrorReceiver::receive_thread(uint16_t port) {
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    running_.store(false);
    return;
  }

  // RAII-style cleanup lambda for exception safety
  auto cleanup_socket = [&sock]() {
    if (sock != INVALID_SOCKET) {
      closesocket(sock);
      sock = INVALID_SOCKET;
    }
  };

  // Allow port reuse to prevent "bind failed" after unclean shutdown
  {
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) == SOCKET_ERROR) {
      MLOG_ERROR("mirror", "setsockopt(SO_REUSEADDR) failed");
      // Non-fatal, continue
    }
  }

#ifdef _WIN32
  DWORD tv = 10;
  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) == SOCKET_ERROR) {
    MLOG_ERROR("mirror", "setsockopt(SO_RCVTIMEO) failed");
    // Non-fatal, continue
  }
#else
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 10000;
  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == SOCKET_ERROR) {
    MLOG_ERROR("mirror", "setsockopt(SO_RCVTIMEO) failed");
  }
#endif

  int bufsize = 4 * 1024 * 1024;
  if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize)) == SOCKET_ERROR) {
    MLOG_ERROR("mirror", "setsockopt(SO_RCVBUF) failed");
    // Non-fatal, continue with default buffer
  }

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);  // port=0 means OS assigns an available port

  if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
    MLOG_ERROR("mirror", "bind() failed on port %d", port);
    cleanup_socket();
    running_.store(false);
    return;
  }

  // Get actual bound port (especially when port=0 was used)
  struct sockaddr_in bound_addr{};
  socklen_t addr_len = sizeof(bound_addr);
  if (getsockname(sock, (struct sockaddr*)&bound_addr, &addr_len) == 0) {
    uint16_t actual_port = ntohs(bound_addr.sin_port);
    bound_port_.store(actual_port);
    MLOG_INFO("mirror", "Listening on UDP port %d", actual_port);
  } else {
    bound_port_.store(port);
    MLOG_INFO("mirror", "Listening on UDP port %d (getsockname failed)", port);
  }

  uint8_t buf[65536];
  while (running_.load()) {
    int len = recvfrom(sock, (char*)buf, sizeof(buf), 0, nullptr, nullptr);
    if (len > 0) {
      bytes_received_.fetch_add(len);
      process_rtp_packet(buf, static_cast<size_t>(len));
    } else if (len < 0) {
      // Error occurred (not timeout)
#ifdef _WIN32
      int err = WSAGetLastError();
      if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
        MLOG_ERROR("mirror", "recvfrom error: %d", err);
      }
#else
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        MLOG_ERROR("mirror", "recvfrom error: %d", errno);
      }
#endif
    }
  }

  closesocket(sock);
}

static inline uint16_t rd16(const uint8_t* p) {
  return (uint16_t(p[0]) << 8) | p[1];
}

// ==============================================================================
// TCP receive mode - connects directly to MirageCapture TcpVideoSender via ADB forward
// Eliminates UDP packet loss that causes green screen / block artifacts
// ==============================================================================
bool MirrorReceiver::start_tcp(uint16_t tcp_port) {
  tcp_port_ = tcp_port;
  if (!init_decoder()) return false;
  running_.store(true);
  bound_port_.store(tcp_port);  // Report the TCP port as "bound"
  thread_ = std::thread(&MirrorReceiver::tcp_receive_thread, this, tcp_port);
  decode_thread_ = std::thread(&MirrorReceiver::decode_thread_func, this);
  return true;
}

void MirrorReceiver::tcp_receive_thread(uint16_t tcp_port) {
  MLOG_INFO("mirror", "TCP receive thread: connecting to localhost:%d", tcp_port);

#ifdef _WIN32
  SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == INVALID_SOCKET) {
    MLOG_ERROR("mirror", "TCP socket creation failed");
    running_.store(false);
    return;
  }

  // TCP_NODELAY for low latency
  int nodelay = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

  // Large receive buffer
  int bufsize = 4 * 1024 * 1024;
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(tcp_port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  // Retry connect (MirageCapture service may still be starting)
  bool connected = false;
  for (int i = 0; i < 30 && running_.load(); i++) {
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  if (!connected) {
    MLOG_ERROR("mirror", "TCP connect to localhost:%d failed after retries", tcp_port);
    closesocket(sock);
    running_.store(false);
    return;
  }

  MLOG_INFO("mirror", "TCP connected on port %d", tcp_port);

  // Set recv timeout so we can check running_ flag
  DWORD tv = 100;  // 100ms
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

  char buf[65536];
  while (running_.load()) {
    int n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
      bytes_received_.fetch_add(n);
      process_raw_h264(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
    } else if (n == 0) {
      MLOG_WARN("mirror", "TCP connection closed by server");
      break;
    } else {
      int err = WSAGetLastError();
      if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
        MLOG_ERROR("mirror", "TCP recv error: %d", err);
        break;
      }
    }
  }

  closesocket(sock);
#endif
  MLOG_INFO("mirror", "TCP receive thread ended");
}

// ==============================================================================
// VID0 TCP receive mode - MirageCapture TcpVideoSender (port 50100)
// VID0 format: "VID0" (4B) + payload_length (4B big-endian) + RTP packet
// ==============================================================================
bool MirrorReceiver::start_tcp_vid0(uint16_t tcp_port, const std::string& host) {
  tcp_port_ = tcp_port;
  tcp_host_ = host;
  if (!init_decoder()) return false;
  running_.store(true);
  bound_port_.store(tcp_port);
  thread_ = std::thread(&MirrorReceiver::tcp_vid0_receive_thread, this, tcp_port);
  decode_thread_ = std::thread(&MirrorReceiver::decode_thread_func, this);
  return true;
}

void MirrorReceiver::tcp_vid0_receive_thread(uint16_t tcp_port) {
  MLOG_INFO("mirror", "VID0 TCP receive thread started (port %d, host %s, auto-reconnect enabled)", tcp_port, tcp_host_.c_str());

#ifdef _WIN32
  // Outer reconnection loop: reconnects indefinitely until running_ is false
  while (running_.load()) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
      MLOG_ERROR("mirror", "VID0 TCP socket creation failed, retrying in 3s");
      std::this_thread::sleep_for(std::chrono::seconds(3));
      continue;
    }

    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

    int bufsize = 128 * 1024;  // 128KB~128ms@8Mbps (was 4MB=~4s of stale frames)
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tcp_port);
    inet_pton(AF_INET, tcp_host_.c_str(), &addr.sin_addr);

    // Connection retry (wait for MirageCapture to start accepting)
    bool connected = false;
    for (int i = 0; i < 50 && running_.load(); i++) {
      if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
        connected = true;
        break;
      }
      if (i % 10 == 9) {
        MLOG_INFO("mirror", "VID0 TCP connect retry %d/50 (port %d)", i+1, tcp_port);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (!connected) {
      MLOG_WARN("mirror", "VID0 TCP connect failed (port %d), retrying in 3s...", tcp_port);
      closesocket(sock);
      std::this_thread::sleep_for(std::chrono::seconds(3));
      continue;  // Outer loop: retry connection indefinitely
    }

    // CODEC RESET: clear stream state on every new TCP connection
    // (prevents HEVC false-positive from previous connection polluting new AVC stream)
    {
        stream_is_hevc_ = false;
        cached_sps_.clear();
        cached_pps_.clear();
        cached_vps_.clear();
        has_valid_sps_ = false;
        need_idr_.store(true);
        // Re-initialize decoder for H.264
        if (unified_decoder_) { unified_decoder_->destroy(); unified_decoder_.reset(); }
        if (decoder_) { decoder_.reset(); }
        init_decoder();
        MLOG_INFO("mirror", "VID0 codec state reset for new connection (port %d)", tcp_port);
    }
    // CODEC RESET: clear HEVC/AVC detection state on every new TCP connection
    {
        stream_is_hevc_ = false;
        cached_sps_.clear(); cached_pps_.clear(); cached_vps_.clear();
        has_valid_sps_ = false;
        need_idr_.store(true);
        if (unified_decoder_) { unified_decoder_->destroy(); unified_decoder_.reset(); }
        if (decoder_) { decoder_.reset(); }
        init_decoder();
        MLOG_INFO("mirror", "VID0 codec state reset (H264, port %d)", tcp_port);
    }
    // LATENCY FIX: drain any stale OS TCP buffer data after connect
    { DWORD nb=1; ioctlsocket(sock,FIONBIO,&nb); char db[32768]; int tot=0,dr;
      while((dr=recv(sock,db,sizeof(db),0))>0) tot+=dr;
      nb=0; ioctlsocket(sock,FIONBIO,&nb);
      if(tot>0) MLOG_INFO("mirror","VID0 stale drain: %d bytes (port %d)",tot,tcp_port); }
    MLOG_INFO("mirror", "VID0 TCP connected on port %d host %s (MirageCapture)", tcp_port, tcp_host_.c_str());

    DWORD tv = 100;  // 100ms recv timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    std::vector<uint8_t> vid0_buf;
    vid0_buf.reserve(256 * 1024);

    char buf[65536];
    while (running_.load()) {
      int n = recv(sock, buf, sizeof(buf), 0);
      if (n > 0) {
        bytes_received_.fetch_add(n);
        vid0_buf.insert(vid0_buf.end(), buf, buf + n);

        // Parse VID0 framing to extract RTP packets
        auto parse_result = mirage::video::parseVid0Packets(vid0_buf);

        // Update last parse stats for discontinuity diagnostics
        last_vid0_recv_n_.store(n);
        last_vid0_buf_size_.store(vid0_buf.size());
        last_vid0_rtp_count_.store((int)parse_result.rtp_packets.size());
        last_vid0_sync_errors_.store(parse_result.sync_errors);
        last_vid0_resync_.store(parse_result.magic_resync);
        last_vid0_invalid_len_.store(parse_result.invalid_len);

        // Periodic VID0/TCP stats (per port)
        // Time-based VID0/TCP stats (per port)
        static thread_local auto last_stat_t = std::chrono::steady_clock::now();
        static thread_local uint64_t last_bytes = 0;
        static thread_local uint64_t last_pkts = 0;
        static thread_local uint64_t last_disc = 0;
        static thread_local uint64_t last_gap = 0;
        auto now_t = std::chrono::steady_clock::now();
        auto dt_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now_t - last_stat_t).count();
        if (dt_ms >= 10000) {
          uint64_t b = bytes_received_.load();
          uint64_t p = packets_received_.load();
          uint64_t d = discontinuities_.load();
          uint64_t g = gaps_detected_.load();
          double mbps = (dt_ms > 0) ? ((double)(b - last_bytes) * 8.0 / 1e6) / ((double)dt_ms / 1000.0) : 0.0;
          MLOG_INFO("mirror", "VID0 stats port %d: mbps=%.2f rtp=%zu recv=%d buf=%zu sync=%d resync=%d invalid=%d disc=%llu gap=%llu",
                    tcp_port, mbps, parse_result.rtp_packets.size(), n, vid0_buf.size(),
                    parse_result.sync_errors, parse_result.magic_resync, parse_result.invalid_len,
                    (unsigned long long)(d - last_disc), (unsigned long long)(g - last_gap));
          last_stat_t = now_t;
          last_bytes = b;
          last_pkts = p;
          last_disc = d;
          last_gap = g;
        }

        // VID0 parser health check (helps diagnose FU-A gaps on TCP)
        if (parse_result.sync_errors > 0 || parse_result.buffer_overflow) {
          MLOG_WARN("mirror", "VID0 parser anomalies: sync_errors=%d resync=%d invalid_len=%d overflow=%d buf=%zu (port %d)",
                    parse_result.sync_errors, parse_result.magic_resync, parse_result.invalid_len, (int)parse_result.buffer_overflow, vid0_buf.size(), tcp_port);
          // If overflow happened, drop buffer to resync quickly (avoid endless FU-A gaps)
          if (parse_result.buffer_overflow) {
            vid0_buf.clear();
          }
        }


        for (const auto& rtp_pkt : parse_result.rtp_packets) {
          packets_received_.fetch_add(1);
          process_rtp_packet(rtp_pkt.data(), rtp_pkt.size());
        }
      } else if (n == 0) {
        MLOG_WARN("mirror", "VID0 TCP connection closed by server (port %d)", tcp_port);
        break;  // Inner loop exit -> reconnect via outer loop
      } else {
        int err = WSAGetLastError();
        if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
          MLOG_ERROR("mirror", "VID0 TCP recv error: %d (port %d)", err, tcp_port);
          break;  // Inner loop exit -> reconnect via outer loop
        }
      }
    }

    closesocket(sock);

    if (running_.load()) {
      MLOG_INFO("mirror", "VID0 TCP disconnected, reconnecting in 2s (port %d)", tcp_port);
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }  // End outer reconnection loop
#endif
  MLOG_INFO("mirror", "VID0 TCP receive thread ended (port %d)", tcp_port);
}


// ==============================================================================
// Raw H.264 Annex B stream processing (Annex B input path)
// Accumulates UDP chunks and extracts NAL units at start code boundaries
// ==============================================================================
void MirrorReceiver::process_raw_h264(const uint8_t* data, size_t len) {
  // Append to accumulation buffer
  raw_h264_buf_.insert(raw_h264_buf_.end(), data, data + len);

  // Extract complete NAL units (delimited by 00 00 00 01)
  while (raw_h264_buf_.size() >= 4) {
    // Find first start code
    size_t first_sc = find_start_code(raw_h264_buf_.data(), raw_h264_buf_.size(), 0);
    if (first_sc == (size_t)-1) {
      // No start code found - discard accumulated junk
      raw_h264_buf_.clear();
      return;
    }
    if (first_sc > 0) {
      // Discard bytes before first start code
      raw_h264_buf_.erase(raw_h264_buf_.begin(), raw_h264_buf_.begin() + first_sc);
    }

    // Determine start code length (3 or 4 bytes)
    size_t sc_len = (raw_h264_buf_.size() >= 4 &&
                     raw_h264_buf_[0] == 0 && raw_h264_buf_[1] == 0 &&
                     raw_h264_buf_[2] == 0 && raw_h264_buf_[3] == 1) ? 4 : 3;

    // Find next start code (end of current NAL)
    size_t next_sc = find_start_code(raw_h264_buf_.data(), raw_h264_buf_.size(), sc_len);
    if (next_sc == (size_t)-1) {
      // No second start code - NAL is incomplete, wait for more data
      // Safety: if buffer is huge (>1MB), something is wrong - flush it
      if (raw_h264_buf_.size() > 1024 * 1024) {
        MLOG_WARN("mirror", "Raw H.264 buffer overflow (%zu bytes), flushing + requesting IDR", raw_h264_buf_.size());
        raw_h264_buf_.clear();
        need_idr_.store(true);
        // IDRリクエストスロットル: 最低1500ms間隔
        {
          auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
          auto last = last_idr_request_ms_.load();
          if (now_ms - last >= 1500 && last_idr_request_ms_.compare_exchange_strong(last, now_ms)) {
            if (on_idr_needed_) on_idr_needed_();
          }
        }
      }
      return;
    }

    // Extract NAL unit (without start code prefix)
    const uint8_t* nal_data = raw_h264_buf_.data() + sc_len;
    size_t nal_len = next_sc - sc_len;
    if (nal_len > 0) {
      packets_received_.fetch_add(1);

      // NALタイプデバッグログ（最初の5個は全て、以降はSPS/PPS/IDRのみ）
      uint8_t dbg_nal_type = nal_data[0] & 0x1F;
      if (nal_log_count_ < 5) {
        MLOG_INFO("mirror", "NAL[%zu] type=%d len=%zu", nal_log_count_, dbg_nal_type, nal_len);
      } else if (dbg_nal_type == 7 || dbg_nal_type == 8 || dbg_nal_type == 5) {
        MLOG_INFO("mirror", "NAL[%zu] type=%d len=%zu", nal_log_count_, dbg_nal_type, nal_len);
      }

      enqueue_nal(nal_data, nal_len);
    }

    // Remove processed NAL from buffer
    raw_h264_buf_.erase(raw_h264_buf_.begin(), raw_h264_buf_.begin() + next_sc);
  }
}

// Find H.264 Annex B start code (00 00 00 01 or 00 00 01) starting from offset
size_t MirrorReceiver::find_start_code(const uint8_t* data, size_t len, size_t offset) {
  for (size_t i = offset; i + 3 <= len; i++) {
    if (data[i] == 0 && data[i+1] == 0) {
      if (i + 3 < len && data[i+2] == 0 && data[i+3] == 1) return i;  // 00 00 00 01
      if (data[i+2] == 1) return i;  // 00 00 01
    }
  }
  return (size_t)-1;
}

void MirrorReceiver::process_rtp_packet(const uint8_t* data, size_t len) {
  if (len < 12) return;

  // Check if this is raw H.264 Annex B or RTP
  uint8_t version = (data[0] >> 6) & 0x03;
  if (version != 2) {
    // Not RTP - treat as raw H.264 Annex B stream chunk
    process_raw_h264(data, len);
    return;
  }

  packets_received_.fetch_add(1);

  // RTP受信時刻を更新（フェイルオーバー検知用）
  {
    uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    last_rtp_recv_ms_.store(now_ms, std::memory_order_relaxed);
  }

  uint16_t seq = rd16(data + 2);

  // RTP seq discontinuity monitor (helps diagnose FU-A gaps on TCP)
  uint16_t prev_seq_dbg = last_seq_.load(std::memory_order_relaxed);
  uint16_t prev_plus_one_dbg = static_cast<uint16_t>(prev_seq_dbg + 1);
  if (prev_seq_dbg != 0 && prev_plus_one_dbg != seq) {
    discontinuities_.fetch_add(1);
    // Log occasionally to avoid spam
    uint64_t n = gaps_detected_.load();
    if ((n % 100) == 0) {
      MLOG_WARN("mirror", "RTP seq discontinuity: prev=%u now=%u (port %d) | last_recv=%d vid0_buf=%zu rtp=%d sync=%d resync=%d invalid=%d", (unsigned)prev_seq_dbg, (unsigned)seq, (int)tcp_port_, last_vid0_recv_n_.load(), last_vid0_buf_size_.load(), last_vid0_rtp_count_.load(), last_vid0_sync_errors_.load(), last_vid0_resync_.load(), last_vid0_invalid_len_.load());
    }
  }


  // Track sequence for FU-A continuity (must be done BEFORE processing)
  last_seq_.store(seq, std::memory_order_relaxed);


  uint8_t cc = data[0] & 0x0F;
  bool has_extension = (data[0] & 0x10) != 0;

  size_t header_len = 12 + (cc * 4);
  if (len < header_len) return;

  if (has_extension) {
    if (len < header_len + 4) return;
    uint16_t ext_len = rd16(data + header_len + 2);
    size_t ext_bytes = 4 + (static_cast<size_t>(ext_len) * 4);
    if (ext_bytes > 65535 || header_len + ext_bytes > len) return;
    header_len += ext_bytes;
  }

  if (len <= header_len) return;

  const uint8_t* payload = data + header_len;
  size_t payload_len = len - header_len;
  if (payload_len < 1) return;

  uint8_t nal_type = payload[0] & 0x1F;

  // ── HEVC RTP dispatch (RFC 7798) ─────────────────────────────────
  // PayloadHdr byte0: nal_type = (byte0 >> 1) & 0x3f
  // type 49 = FU,  0-47 = single NAL unit
  if (payload_len >= 2) {
    int h_pt = (payload[0] >> 1) & 0x3f;
    // Early detect: VPS/SPS/PPS(32/33/34) が来たら即 HEVC モードへ
    if (!stream_is_hevc_ && cached_sps_.empty()
        && (h_pt == 32 || h_pt == 33 || h_pt == 34)
        && (payload[0] & 0x1f) > 23) {  // AVC guard
      stream_is_hevc_ = true;
      has_valid_sps_  = true;
      MLOG_INFO("mirror", "[RTP] Early HEVC detect nal_type=%d", h_pt);
      if (unified_decoder_) { unified_decoder_->destroy(); unified_decoder_.reset(); }
      init_decoder();
    }
    if (stream_is_hevc_) {
      if (h_pt == 49 && payload_len >= 3) {
        // HEVC FU reassembly
        uint8_t fu_hdr  = payload[2];
        bool fu_start   = (fu_hdr & 0x80) != 0;
        bool fu_end     = (fu_hdr & 0x40) != 0;
        uint8_t fu_type = fu_hdr & 0x3f;
        uint8_t hdr0 = (fu_type << 1) & 0xFE;  // forbidden=0, nal_type=fu_type
        uint8_t hdr1 = payload[1];               // layer_id / tid
        if (fu_start) {
          fu_buf_.clear();
          fu_buf_.push_back(hdr0); fu_buf_.push_back(hdr1);
          fu_buf_.insert(fu_buf_.end(), payload + 3, payload + payload_len);
          fu_start_seq_ = seq; fu_last_seq_ = seq;
          fu_have_last_seq_ = true; have_fu_ = true;
        } else if (have_fu_) {
          uint16_t exp = static_cast<uint16_t>(fu_last_seq_ + 1);
          if (seq != exp) {
            MLOG_INFO("mirror", "[HEVC FU] gap exp=%u got=%u", (unsigned)exp, (unsigned)seq);
            have_fu_ = false; fu_buf_.clear(); fu_have_last_seq_ = false;
          } else {
            size_t ns = fu_buf_.size() + (payload_len - 3);
            if (ns > MAX_FU_BUFFER_SIZE) { have_fu_ = false; fu_buf_.clear(); }
            else { fu_buf_.insert(fu_buf_.end(), payload + 3, payload + payload_len); fu_last_seq_ = seq; }
          }
        }
        if (fu_end && have_fu_ && fu_buf_.size() >= 2) {
          enqueue_nal(fu_buf_.data(), fu_buf_.size());
          nals_received_.fetch_add(1);
          have_fu_ = false; fu_buf_.clear(); fu_have_last_seq_ = false;
        }
      } else if (h_pt <= 47) {
        // HEVC single NAL unit
        enqueue_nal(payload, payload_len);
        nals_received_.fetch_add(1);
      }
      return;  // HEVC handled; skip H.264 path below
    }
  }

  if (nal_type >= 1 && nal_type <= 23) {
    enqueue_nal(payload, payload_len);
    nals_received_.fetch_add(1);
  }
  else if (nal_type == 24) {
    size_t p = 1;
    while (p + 2 <= payload_len) {
      uint16_t sz = rd16(payload + p);
      p += 2;
      if (p + sz > payload_len) break;
      enqueue_nal(payload + p, sz);
      nals_received_.fetch_add(1);
      p += sz;
    }
  }
  else if (nal_type == 28) {
    // FU-A (fragmentation unit)
    if (payload_len < 2) return;

    uint8_t fu_header = payload[1];
    bool start = (fu_header & 0x80) != 0;
    bool end = (fu_header & 0x40) != 0;
    uint8_t real_type = fu_header & 0x1F;
    uint8_t nri = payload[0] & 0x60;

    if (start) {
      fu_buf_.clear();
      fu_buf_.push_back(nri | real_type);
      fu_buf_.insert(fu_buf_.end(), payload + 2, payload + payload_len);
      fu_start_seq_ = seq;
      fu_last_seq_ = seq;
      fu_have_last_seq_ = true;
      have_fu_ = true;
    } else if (have_fu_) {
      // Strict FU-A assembly: if sequence is not contiguous, drop ONLY this fragmented NAL and wait for next.
      uint16_t expected = static_cast<uint16_t>(fu_last_seq_ + 1);
      if (seq != expected) {
        gaps_detected_.fetch_add(1);
        MLOG_INFO("mirror", "[FU-A] Gap! expected=%u got=%u -> drop this NAL",
                  (unsigned)expected, (unsigned)seq);
        have_fu_ = false;
        fu_buf_.clear();
        fu_have_last_seq_ = false;
        // IDR要求コールバック（FU-Aギャップからの回復用、スロットル付き）
        {
          auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
          auto last = last_idr_request_ms_.load();
          if (now_ms - last >= 3000 && last_idr_request_ms_.compare_exchange_strong(last, now_ms)) {
            if (on_idr_needed_) on_idr_needed_();
          }
        }
        // Do not enter global drop-until-IDR mode; just abandon this NAL.
        goto fu_done;
      }

      size_t new_size = fu_buf_.size() + (payload_len - 2);
      if (new_size > MAX_FU_BUFFER_SIZE) {
        gaps_detected_.fetch_add(1);
        MLOG_INFO("mirror", "[FU-A] Buffer overflow! size=%zu -> drop this NAL", new_size);
        have_fu_ = false;
        fu_buf_.clear();
        fu_have_last_seq_ = false;
      } else {
        fu_buf_.insert(fu_buf_.end(), payload + 2, payload + payload_len);
        fu_last_seq_ = seq;
        fu_have_last_seq_ = true;
      }
    }

    fu_done:
    if (end && have_fu_) {
      if (fu_buf_.size() > 1) {
        enqueue_nal(fu_buf_.data(), fu_buf_.size());
        nals_received_.fetch_add(1);
      }
      have_fu_ = false;
      fu_buf_.clear();
      fu_have_last_seq_ = false;
    }
  }
}

void MirrorReceiver::enqueue_nal(const uint8_t* data, size_t len) {
  if (len < 1) return;

  // HEVC early-detection: detect HEVC before queuing so VPS/SPS/PPS are not dropped
  // decode_nal also detects HEVC but is called after dequeue - too late for first NALs
  if (!stream_is_hevc_ && cached_sps_.empty() && len >= 2) {
    int early_hevc_type = (data[0] >> 1) & 0x3f;
    if (early_hevc_type == 32 || early_hevc_type == 33 || early_hevc_type == 34) {
      stream_is_hevc_ = true;
      MLOG_INFO("mirror", "[enqueue_nal] HEVC early-detect: nal_type=%d - switching to HEVC", early_hevc_type);
      if (unified_decoder_) { unified_decoder_->destroy(); unified_decoder_.reset(); }
      init_decoder();
      has_valid_sps_ = true;
    }
  }

  // HEVC: H.264 SPSゲートをバイパスしてキューに直接積む
  if (stream_is_hevc_) {
    std::lock_guard<std::mutex> lk(nal_queue_mtx_);
    if (nal_queue_.size() >= MAX_NAL_QUEUE_SIZE) {
      int ht = (len >= 2) ? ((data[0] >> 1) & 0x3f) : -1;
      if (ht == 19 || ht == 20) {  // IDR: staleをフラッシュして受け入れ
        size_t n = nal_queue_.size();
        while (!nal_queue_.empty()) nal_queue_.pop();
        MLOG_WARN("mirror", "[enqueue_nal HEVC] IDR flush: %zu stale NALs", n);
      } else { return; }
    }
    NalUnit nal; nal.data.assign(data, data + len);
    nal_queue_.push(std::move(nal));
    nal_queue_cv_.notify_one();
    return;
  }

  uint8_t nal_type = data[0] & 0x1F;

  // === SPS/PPS validation gate ===
  if (nal_type == 7) {
    // SPS: 次元をパースして妥当性チェック
    int w = 0, h = 0;
    if (parse_sps_dimensions(data, len, w, h)) {
      // アスペクト比チェック（4:1超は異常）
      bool sane = (w >= 320 && w <= 4096 && h >= 320 && h <= 4096);
      if (sane && w > 0 && h > 0) {
        double ratio = (w > h) ? static_cast<double>(w) / h : static_cast<double>(h) / w;
        sane = (ratio < 4.0);
      }
      if (sane) {
        if (!has_valid_sps_ || sps_width_ != w || sps_height_ != h) {
          MLOG_INFO("mirror", "Valid SPS: %dx%d (len=%zu)", w, h, len);
        }
        has_valid_sps_ = true;
        sps_width_ = w;
        sps_height_ = h;
      } else {
        MLOG_WARN("mirror", "Invalid SPS dimensions: %dx%d, dropping frames until valid SPS", w, h);
        has_valid_sps_ = false;
        return;  // 無効SPSはデコーダに渡さない
      }
    } else {
      MLOG_WARN("mirror", "SPS parse failed (len=%zu), dropping frames until valid SPS", len);
      has_valid_sps_ = false;
      return;
    }
  } else if (nal_type == 8) {
    // PPS: 常にパススルー（キャッシュはdecode_nalで実施）
  } else {
    // IDR(5) / 非IDR(1) / その他: has_valid_sps_がないとドロップ
    if (!has_valid_sps_) {
      if (nal_log_count_ < 5 || (nal_log_count_ % 100 == 0)) {
        MLOG_INFO("mirror", "Dropping NAL type=%d (no valid SPS yet, count=%zu)", nal_type, nal_log_count_);
      }
      nal_log_count_++;
      return;
    }
  }

  {
    std::lock_guard<std::mutex> lock(nal_queue_mtx_);
    if (nal_queue_.size() >= MAX_NAL_QUEUE_SIZE) {
      // IDRが来たらキュー全クリアして先頭に置く（I-frame優先）
      if (nal_type == 5) {
        while (!nal_queue_.empty()) nal_queue_.pop();
        MLOG_WARN("mirror", "[enqueue_nal] Queue full: IDR arrived, flushed %zu stale NALs",
                  (size_t)MAX_NAL_QUEUE_SIZE);
      } else {
        // 非IDRは捨てる（古いIDRを守る）
        MLOG_DEBUG("mirror", "[enqueue_nal] Queue full: dropping NAL type=%d", nal_type);
        return;
      }
    }
    NalUnit nal;
    nal.data.reserve(len);
    nal.data.assign(data, data + len);
    nal_queue_.push(std::move(nal));
  }
  nal_queue_cv_.notify_one();
}

void MirrorReceiver::decode_thread_func() {
  MLOG_INFO("mirror", "Decode thread started");
  std::vector<NalUnit> batch;
  batch.reserve(32);

  while (running_.load()) {
    batch.clear();

    // Apply requested decoder flush/reset in the decode thread context.
    if (request_decoder_flush_.exchange(false)) {
      MLOG_WARN("mirror", "Decoder flush requested (gap recovery)");
      if (unified_decoder_) unified_decoder_->flush();
#ifdef USE_FFMPEG
      if (decoder_) decoder_->flush();
#endif
      // SPS状態もリセット: flush後は再度SPS受信まで待つ
      has_valid_sps_ = false;
      sps_logged_ = false;
      pps_logged_ = false;
      cached_sps_.clear();
      cached_pps_.clear();
    }

    {
      std::unique_lock<std::mutex> lock(nal_queue_mtx_);
      nal_queue_cv_.wait_for(lock, std::chrono::milliseconds(2), [this] {
        return !nal_queue_.empty() || !running_.load();
      });
      if (!running_.load() && nal_queue_.empty()) break;

      // HEVC は GOP が大きいため閾値を128に拡大。H.264 は従来の8を維持。
      const size_t DROP_THRESHOLD = stream_is_hevc_ ? 128 : 3;  // H264@60fps: 3 NAL≈50ms (was 8≈133ms)
      if (nal_queue_.size() > DROP_THRESHOLD) {
        size_t dropped = 0;
        bool found_key = false;
        while (nal_queue_.size() > 2) {
          const auto& front = nal_queue_.front();
          bool is_key = false;
          if (front.data.size() > 0) {
            if (stream_is_hevc_ && front.data.size() >= 2) {
              int ht = (front.data[0] >> 1) & 0x3f;
              is_key = (ht==19||ht==20||ht==32||ht==33||ht==34);
            } else {
              int at = front.data[0] & 0x1f;
              is_key = (at==5||at==7||at==8);
            }
          }
          if (is_key) { found_key = true; break; }
          nal_queue_.pop();
          dropped++;
        }
        if (dropped > 0) {
          MLOG_INFO("mirror", "Queue drop: %zu frames (thr=%zu stream=%s low-latency) key=%d",
            dropped, DROP_THRESHOLD, stream_is_hevc_ ? "HEVC" : "H264", found_key ? 1 : 0);
          // found_key=true: IDR is at queue front, resume immediately
          // found_key=false: no IDR found, wait for next IDR to avoid corruption
          need_idr_.store(!found_key);
        }
      }

      while (!nal_queue_.empty()) {
        batch.push_back(std::move(nal_queue_.front()));
        nal_queue_.pop();
      }
    }
    for (auto& nal : batch) {
      decode_nal(nal.data.data(), nal.data.size());
    }
  }
  // Drain remaining
  std::lock_guard<std::mutex> lock(nal_queue_mtx_);
  while (!nal_queue_.empty()) {
    auto& nal = nal_queue_.front();
    decode_nal(nal.data.data(), nal.data.size());
    nal_queue_.pop();
  }
  MLOG_INFO("mirror", "Decode thread ended");
}

void MirrorReceiver::decode_nal(const uint8_t* data, size_t len) {
  if (len < 1) return;

  // Auto-detect HEVC by NAL unit type (VPS/SPS/PPS are 32/33/34 in HEVC)
  // HEVC nal_type = (byte0 >> 1) & 0x3f
  // Guard: if H.264 SPS already cached the stream is AVC - skip HEVC detection.
  {
    int hevc_nal_type = (len >= 2) ? ((data[0] >> 1) & 0x3f) : -1;
    bool avc_sps_seen = !cached_sps_.empty();
    if (!stream_is_hevc_ && !avc_sps_seen && (hevc_nal_type == 32 || hevc_nal_type == 33 || hevc_nal_type == 34)) {
      stream_is_hevc_ = true;
      MLOG_INFO("mirror", "HEVC VPS/SPS detected (nal_type=%d) - switching decoder to HEVC", hevc_nal_type);
      if (unified_decoder_) { unified_decoder_->destroy(); unified_decoder_.reset(); }
      init_decoder();
      has_valid_sps_ = true; // bypass H.264 SPS gate
    }
    // HEVC fast decode path
    if (stream_is_hevc_) {
      // VPS/SPS/PPS をキャッシュ（IDR prepend 用）
      if (hevc_nal_type == 32)      { cached_vps_.assign(data, data + len); return; }
      else if (hevc_nal_type == 33) { cached_sps_.assign(data, data + len); return; }
      else if (hevc_nal_type == 34) { cached_pps_.assign(data, data + len); return; }
      // IDR なら VPS+SPS+PPS を先頭に付けて Annex-B に変換
      if (annexb_buf_.capacity() == 0) annexb_buf_.reserve(128 * 1024);
      annexb_buf_.clear();
      if ((hevc_nal_type == 19 || hevc_nal_type == 20)
          && !cached_sps_.empty() && !cached_pps_.empty()) {
        auto append = [&](const std::vector<uint8_t>& v) {
          const uint8_t sc[4] = {0,0,0,1};
          annexb_buf_.insert(annexb_buf_.end(), sc, sc+4);
          annexb_buf_.insert(annexb_buf_.end(), v.begin(), v.end());
        };
        if (!cached_vps_.empty()) append(cached_vps_);
        append(cached_sps_);
        append(cached_pps_);
      }
      { const uint8_t sc[4]={0,0,0,1};
        annexb_buf_.insert(annexb_buf_.end(), sc, sc+4);
        annexb_buf_.insert(annexb_buf_.end(), data, data+len); }
      if (use_unified_decoder_ && unified_decoder_)
        unified_decoder_->decode(annexb_buf_.data(), annexb_buf_.size(), 0);
      return;
    }
  }


  uint8_t nal_type = data[0] & 0x1F;

  // Cache SPS/PPS for stream recovery
  if (nal_type == 7 && len >= 8 && len <= MAX_SPS_SIZE) {
    cached_sps_.assign(data, data + len);
    if (!sps_logged_) {
      sps_logged_ = true;
      MLOG_INFO("mirror", "Cached SPS len=%zu", len);
    }
  } else if (nal_type == 8 && len >= 2 && len <= MAX_PPS_SIZE) {
    cached_pps_.assign(data, data + len);
    if (!pps_logged_) {
      pps_logged_ = true;
      MLOG_INFO("mirror", "Cached PPS len=%zu", len);
    }
  }

  // Skip standalone SPS/PPS - they are cached above and will be prepended to IDR
  if (nal_type == 7 || nal_type == 8) return;

  // If we're recovering from packet loss, drop everything until next IDR.
  if (need_idr_.load() && nal_type != 5) {
    return;
  }
  if (need_idr_.load() && nal_type == 5) {
    // We got an IDR, resume decoding.
    need_idr_.store(false);
    MLOG_WARN("mirror", "Recovery: IDR received, resume decoding");
  }

  // Lazy-init reusable buffer
  if (annexb_buf_.capacity() == 0) {
    annexb_buf_.reserve(64 * 1024);
  }
  annexb_buf_.clear();

  // If IDR and we have cached SPS/PPS, concatenate SPS+PPS+IDR into one buffer
  if (nal_type == 5 && !cached_sps_.empty() && !cached_pps_.empty()) {
    // startcode + SPS
    annexb_buf_.push_back(0x00); annexb_buf_.push_back(0x00);
    annexb_buf_.push_back(0x00); annexb_buf_.push_back(0x01);
    annexb_buf_.insert(annexb_buf_.end(), cached_sps_.begin(), cached_sps_.end());
    // startcode + PPS
    annexb_buf_.push_back(0x00); annexb_buf_.push_back(0x00);
    annexb_buf_.push_back(0x00); annexb_buf_.push_back(0x01);
    annexb_buf_.insert(annexb_buf_.end(), cached_pps_.begin(), cached_pps_.end());
  }

  // startcode + NAL data
  annexb_buf_.push_back(0x00); annexb_buf_.push_back(0x00);
  annexb_buf_.push_back(0x00); annexb_buf_.push_back(0x01);
  annexb_buf_.insert(annexb_buf_.end(), data, data + len);

  if (use_unified_decoder_ && unified_decoder_) {
    unified_decoder_->decode(annexb_buf_.data(), annexb_buf_.size(), 0);
    return;
  }

#ifdef USE_FFMPEG
  if (decoder_) {
    decoder_->decode(annexb_buf_.data(), annexb_buf_.size());
    return;
  }
#endif

  if (nal_type == 5 || nal_type == 1) {
    generate_test_frame(640, 480);
  }
}

void MirrorReceiver::on_unified_frame(const uint8_t* rgba, int width, int height, int64_t pts) {
  const int64_t decode_ts = gui::FrameRingBuffer::now_us();  // A-1: capture decode completion time
  std::lock_guard<std::mutex> lock(frame_mtx_);

  const size_t frame_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

  // Copy once into SharedFrame (FFmpeg buffer expires after callback)
  auto sf = std::make_shared<mirage::SharedFrame>();
  sf->width    = width;
  sf->height   = height;
  sf->frame_id = ++test_frame_id_;
  sf->pts_us   = static_cast<uint64_t>(pts);
  if (frame_bytes > 0) {
    // Use buffer pool if available and size matches, else fallback to new
    std::shared_ptr<uint8_t[]> buf;
    if (frame_pool_ && frame_pool_->buffer_size() == frame_bytes) {
      buf = frame_pool_->acquire();
    } else {
      buf = std::shared_ptr<uint8_t[]>(new uint8_t[frame_bytes]);
    }
    std::memcpy(buf.get(), rgba, frame_bytes);
    sf->rgba = std::shared_ptr<const uint8_t[]>(std::move(buf));
  }
  current_shared_frame_ = sf;  // Legacy path (kept for backward compat)
  cur_width_    = width;
  cur_height_   = height;
  cur_frame_id_ = sf->frame_id;
  cur_pts_us_   = sf->pts_us;

  // A-1: Push to ring buffer (primary path)
  if (ring_buffer_) {
    ring_buffer_->push(sf, decode_ts);
  }

  has_new_frame_ = true;
  frames_decoded_.fetch_add(1);

  // Initialize pool on first frame with known resolution
  if (!frame_pool_ && frame_bytes > 0) {
    frame_pool_ = std::make_unique<FrameBufferPool>(frame_bytes, 8);
    MLOG_INFO("mirror", "FrameBufferPool initialized: %zu bytes x 8", frame_bytes);
    if (!ring_buffer_) {
      ring_buffer_ = std::make_unique<gui::FrameRingBuffer>(4);
      MLOG_INFO("mirror", "FrameRingBuffer initialized: capacity=4");
    }
  }

  static bool first_unified_frame = true;
  if (first_unified_frame) {
    first_unified_frame = false;
    MLOG_INFO("mirror", "First UnifiedDecoder frame: %dx%d", width, height);
  }
}

#ifdef USE_FFMPEG
void MirrorReceiver::on_decoded_frame(const uint8_t* rgba, int width, int height, uint64_t pts) {
  const int64_t decode_ts = gui::FrameRingBuffer::now_us();  // A-1: capture decode completion time
  std::lock_guard<std::mutex> lock(frame_mtx_);

  const size_t frame_bytes = static_cast<size_t>(width) * height * 4;

  // Copy once into SharedFrame
  auto sf = std::make_shared<mirage::SharedFrame>();
  sf->width    = width;
  sf->height   = height;
  sf->frame_id = ++test_frame_id_;
  sf->pts_us   = pts;
  if (frame_bytes > 0) {
    // Use buffer pool if available and size matches, else fallback to new
    std::shared_ptr<uint8_t[]> buf;
    if (frame_pool_ && frame_pool_->buffer_size() == frame_bytes) {
      buf = frame_pool_->acquire();
    } else {
      buf = std::shared_ptr<uint8_t[]>(new uint8_t[frame_bytes]);
    }
    std::memcpy(buf.get(), rgba, frame_bytes);
    sf->rgba = std::shared_ptr<const uint8_t[]>(std::move(buf));
  }
  current_shared_frame_ = sf;  // Legacy path (kept for backward compat)
  cur_width_    = width;
  cur_height_   = height;
  cur_frame_id_ = sf->frame_id;
  cur_pts_us_   = sf->pts_us;

  // A-1: Push to ring buffer (primary path)
  if (ring_buffer_) {
    ring_buffer_->push(sf, decode_ts);
  }

  has_new_frame_ = true;
  frames_decoded_.fetch_add(1);

  // Initialize pool on first frame with known resolution
  if (!frame_pool_ && frame_bytes > 0) {
    frame_pool_ = std::make_unique<FrameBufferPool>(frame_bytes, 8);
    MLOG_INFO("mirror", "FrameBufferPool initialized: %zu bytes x 8", frame_bytes);
  }

  static bool first_frame = true;
  if (first_frame) {
    first_frame = false;
    MLOG_INFO("mirror", "First decoded frame: %dx%d", width, height);
  }
}
#endif

void MirrorReceiver::generate_test_frame(int w, int h) {
  std::lock_guard<std::mutex> lock(frame_mtx_);

  cur_width_ = w;
  cur_height_ = h;
  cur_frame_id_ = ++test_frame_id_;
  cur_pts_us_ = test_frame_id_ * 33333;

  const size_t tf_bytes = static_cast<size_t>(w) * h * 4;
  auto tf_buf = std::shared_ptr<uint8_t[]>(new uint8_t[tf_bytes]);
  uint8_t* px = tf_buf.get();
  int bar_width = w / 8;

  uint8_t colors[8][4] = {
    {255, 255, 255, 255},
    {255, 255, 0, 255},
    {0, 255, 255, 255},
    {0, 255, 0, 255},
    {255, 0, 255, 255},
    {255, 0, 0, 255},
    {0, 0, 255, 255},
    {0, 0, 0, 255},
  };

  int offset = (int)(test_frame_id_ % 100);

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int bar = ((x + offset) % w) / bar_width;
      if (bar > 7) bar = 7;

      px[0] = colors[bar][0];
      px[1] = colors[bar][1];
      px[2] = colors[bar][2];
      px[3] = colors[bar][3];
      px += 4;
    }
  }

  auto tf_sf = std::make_shared<mirage::SharedFrame>();
  tf_sf->width    = w;
  tf_sf->height   = h;
  tf_sf->frame_id = cur_frame_id_;
  tf_sf->pts_us   = cur_pts_us_;
  tf_sf->rgba     = std::shared_ptr<const uint8_t[]>(std::move(tf_buf));
  current_shared_frame_ = std::move(tf_sf);
  has_new_frame_ = true;
  frames_decoded_.fetch_add(1);
}

void MirrorReceiver::feed_rtp_packet(const uint8_t* data, size_t len) {
  bytes_received_.fetch_add(len);
  process_rtp_packet(data, len);
}

} // namespace gui
