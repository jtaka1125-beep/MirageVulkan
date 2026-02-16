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

namespace gui {

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
  // Try UnifiedDecoder first (Vulkan Video with FFmpeg fallback)
  if (vk_device_ != VK_NULL_HANDLE) {
    unified_decoder_ = std::make_unique<mirage::video::UnifiedDecoder>();

    mirage::video::UnifiedDecoderConfig config;
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
    config.max_width = 1920;
    config.max_height = 1080;

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
  if (!decoder_->init()) {
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
  out = current_frame_;
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
      // Process UDP packet for WiFi fallback mode
      // In hybrid mode, USB AOA is preferred, but WiFi packets are still processed
      // when available for redundancy/fallback purposes
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
    // len == 0 means connection closed (for TCP), or empty packet for UDP - ignore
  }

  closesocket(sock);
}

static inline uint16_t rd16(const uint8_t* p) {
  return (uint16_t(p[0]) << 8) | p[1];
}

// ==============================================================================
// Raw H.264 Annex B stream processing (for scrcpy raw_stream=true)
// Accumulates UDP chunks and extracts NAL units at start code boundaries
// ==============================================================================
void MirrorReceiver::process_raw_h264(const uint8_t* data, size_t len) {
  // Append to accumulation buffer
  raw_h264_buf_.insert(raw_h264_buf_.end(), data, data + len);
  bytes_received_.fetch_add(len);

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
        MLOG_WARN("mirror", "Raw H.264 buffer overflow (%zu bytes), flushing", raw_h264_buf_.size());
        raw_h264_buf_.clear();
      }
      return;
    }

    // Extract NAL unit (without start code prefix)
    const uint8_t* nal_data = raw_h264_buf_.data() + sc_len;
    size_t nal_len = next_sc - sc_len;
    if (nal_len > 0) {
      packets_received_.fetch_add(1);
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

  // Debug: log first few bytes of first packets
  static std::atomic<int> dbg_count{0};
  int c = dbg_count.fetch_add(1);
  if (c < 5) {
    MLOG_INFO("mirror", "[RTP-DBG] pkt #%d len=%zu bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
              c+1, len,
              data[0], data[1], data[2], data[3],
              data[4], data[5], data[6], data[7],
              data[8], data[9], data[10], data[11]);
  }

  // Check if this is raw H.264 Annex B (from scrcpy bridge) or RTP
  // Raw H.264 starts with 00 00 00 01 or 00 00 01, RTP has version=2 in bits 7-6
  uint8_t version = (data[0] >> 6) & 0x03;
  if (version != 2) {
    // Not RTP - treat as raw H.264 Annex B stream chunk
    process_raw_h264(data, len);
    return;
  }

  packets_received_.fetch_add(1);

  uint16_t seq = rd16(data + 2);

  // Track sequence for FU-A continuity (must be done BEFORE processing)
  uint16_t prev_seq = last_seq_.load(std::memory_order_relaxed);
  last_seq_.store(seq, std::memory_order_relaxed);

  // RTPヘッダー解极E
  // byte 0: V(2), P(1), X(1), CC(4)
  // byte 1: M(1), PT(7)
  uint8_t cc = data[0] & 0x0F;        // CSRC count (max 15)
  bool has_extension = (data[0] & 0x10) != 0;

  size_t header_len = 12 + (cc * 4);  // 基本ヘッダー + CSRC

  // Validate header length doesn't exceed packet
  if (len < header_len) return;

  // 拡張ヘッダーがある場吁E
  if (has_extension) {
    // Need at least 4 more bytes for extension header
    if (len < header_len + 4) return;

    // 拡張ヘッダー長は最後�E2バイト！E2ビット単位！E
    uint16_t ext_len = rd16(data + header_len + 2);

    // Validate extension doesn't cause overflow
    size_t ext_bytes = 4 + (static_cast<size_t>(ext_len) * 4);
    if (ext_bytes > 65535 || header_len + ext_bytes > len) return;

    header_len += ext_bytes;
  }

  if (len <= header_len) return;

  const uint8_t* payload = data + header_len;
  size_t payload_len = len - header_len;

  if (payload_len < 1) return;

  uint8_t nal_type = payload[0] & 0x1F;

  if (nal_type >= 1 && nal_type <= 23) {
    // Single NAL unit packet - reduce logging
    enqueue_nal(payload, payload_len);
    nals_received_.fetch_add(1);
  }
  else if (nal_type == 24) {
    // STAP-A: Single-time aggregation packet (often contains SPS+PPS)
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
    if (payload_len < 2) return;

    uint8_t fu_header = payload[1];
    bool start = (fu_header & 0x80) != 0;
    bool end = (fu_header & 0x40) != 0;
    uint8_t real_type = fu_header & 0x1F;
    uint8_t nri = payload[0] & 0x60;

    // Reduced logging for FU-A

    if (start) {
      // 新しいNAL開姁E
      fu_buf_.clear();
      fu_buf_.push_back(nri | real_type);
      fu_buf_.insert(fu_buf_.end(), payload + 2, payload + payload_len);
      fu_start_seq_ = seq;
      have_fu_ = true;
    } else if (have_fu_) {
      // 継続パケチE�� - シーケンス番号の連続性チェチE���E�ラチE�Eアラウンド対応！E
      // uint16_t同士の比輁E��自動的にラチE�Eアラウンドを処琁E
      uint16_t expected = static_cast<uint16_t>(prev_seq + 1);
      if (seq != expected) {
        MLOG_INFO("mirror", "[FU-A] Gap! expected=%u got=%u, dropping", static_cast<unsigned>(expected), static_cast<unsigned>(seq));
        have_fu_ = false;
        fu_buf_.clear();
      } else {
        // バッファサイズ制限チェチE���E�EoS防止�E�E
        size_t new_size = fu_buf_.size() + (payload_len - 2);
        if (new_size > MAX_FU_BUFFER_SIZE) {
          MLOG_INFO("mirror", "[FU-A] Buffer overflow! size=%zu, dropping NAL", new_size);
          have_fu_ = false;
          fu_buf_.clear();
        } else {
          fu_buf_.insert(fu_buf_.end(), payload + 2, payload + payload_len);
        }
      }
    }

    if (end && have_fu_) {
      if (fu_buf_.size() > 1) {
        enqueue_nal(fu_buf_.data(), fu_buf_.size());
        nals_received_.fetch_add(1);
      }
      have_fu_ = false;
      fu_buf_.clear();
    }
  }
}

void MirrorReceiver::enqueue_nal(const uint8_t* data, size_t len) {
  if (len < 1) return;
  {
    std::lock_guard<std::mutex> lock(nal_queue_mtx_);
    if (nal_queue_.size() >= MAX_NAL_QUEUE_SIZE) {
      // Drop oldest to prevent unbounded growth
      nal_queue_.pop();
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
    {
      std::unique_lock<std::mutex> lock(nal_queue_mtx_);
      nal_queue_cv_.wait_for(lock, std::chrono::milliseconds(2), [this] {
        return !nal_queue_.empty() || !running_.load();
      });
      if (!running_.load() && nal_queue_.empty()) break;
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

  uint8_t nal_type = data[0] & 0x1F;

  static std::atomic<int> dbg_nal_count{0};
  int nc = dbg_nal_count.fetch_add(1);
  if (nc < 10) {
    MLOG_INFO("mirror", "[NAL-DBG] #%d type=%u len=%zu", nc+1, nal_type, len);
  }

  // Cache SPS/PPS for stream recovery (only if size is reasonable)
  // SPS should be at least 8 bytes and within MAX_SPS_SIZE
  // PPS should be at least 2 bytes and within MAX_PPS_SIZE
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

  // Skip standalone SPS/PPS - they are cached above and will be prepended to IDR
  if (nal_type == 7 || nal_type == 8) return;

  // startcode + NAL data
  annexb_buf_.push_back(0x00); annexb_buf_.push_back(0x00);
  annexb_buf_.push_back(0x00); annexb_buf_.push_back(0x01);
  annexb_buf_.insert(annexb_buf_.end(), data, data + len);

  // Use UnifiedDecoder if available
  if (use_unified_decoder_ && unified_decoder_) {
    unified_decoder_->decode(annexb_buf_.data(), annexb_buf_.size(), 0);

    // If no decoded frames yet and we're receiving data, show test frame
    if (frames_decoded_.load() == 0 && packets_received_.load() > 50) {
      static int test_throttle = 0;
      if (++test_throttle % 30 == 0) {
        generate_test_frame(640, 480);
      }
    }
    return;
  }

#ifdef USE_FFMPEG
  if (decoder_) {
    decoder_->decode(annexb_buf_.data(), annexb_buf_.size());

    // If no decoded frames yet and we're receiving data, show test frame
    // This indicates connection is working but decoder is waiting for keyframe
    if (frames_decoded_.load() == 0 && packets_received_.load() > 50) {
      static int test_throttle = 0;
      if (++test_throttle % 30 == 0) {
        generate_test_frame(640, 480);
      }
    }
    return;
  }
#endif

  if (nal_type == 5 || nal_type == 1) {
    generate_test_frame(640, 480);
  }
}

void MirrorReceiver::on_unified_frame(const uint8_t* rgba, int width, int height, int64_t pts) {
  std::lock_guard<std::mutex> lock(frame_mtx_);

  const size_t frame_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

  current_frame_.width = width;
  current_frame_.height = height;
  current_frame_.frame_id = ++test_frame_id_;
  current_frame_.pts_us = static_cast<uint64_t>(pts);

  // Reuse existing buffer if size matches, avoiding reallocation
  if (current_frame_.rgba.size() == frame_bytes) {
    std::memcpy(current_frame_.rgba.data(), rgba, frame_bytes);
  } else {
    current_frame_.rgba.assign(rgba, rgba + frame_bytes);
  }

  has_new_frame_ = true;
  frames_decoded_.fetch_add(1);

  static bool first_unified_frame = true;
  if (first_unified_frame) {
    first_unified_frame = false;
    MLOG_INFO("mirror", "First UnifiedDecoder frame: %dx%d", width, height);
  }
}

#ifdef USE_FFMPEG
void MirrorReceiver::on_decoded_frame(const uint8_t* rgba, int width, int height, uint64_t pts) {
  std::lock_guard<std::mutex> lock(frame_mtx_);

  const size_t frame_bytes = static_cast<size_t>(width) * height * 4;

  current_frame_.width = width;
  current_frame_.height = height;
  current_frame_.frame_id = ++test_frame_id_;
  current_frame_.pts_us = pts;

  // Reuse existing buffer if size matches, avoiding reallocation
  if (current_frame_.rgba.size() == frame_bytes) {
    std::memcpy(current_frame_.rgba.data(), rgba, frame_bytes);
  } else {
    current_frame_.rgba.assign(rgba, rgba + frame_bytes);
  }

  has_new_frame_ = true;
  frames_decoded_.fetch_add(1);

  static bool first_frame = true;
  if (first_frame) {
    first_frame = false;
    MLOG_INFO("mirror", "First decoded frame: %dx%d", width, height);
  }
}
#endif

void MirrorReceiver::generate_test_frame(int w, int h) {
  std::lock_guard<std::mutex> lock(frame_mtx_);

  current_frame_.width = w;
  current_frame_.height = h;
  current_frame_.frame_id = ++test_frame_id_;
  current_frame_.pts_us = test_frame_id_ * 33333;

  current_frame_.rgba.resize(w * h * 4);

  uint8_t* px = current_frame_.rgba.data();
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

  has_new_frame_ = true;
  frames_decoded_.fetch_add(1);
}

void MirrorReceiver::feed_rtp_packet(const uint8_t* data, size_t len) {
  static std::atomic<int> feed_dbg{0};
  int fd = feed_dbg.fetch_add(1);
  if (fd < 5) {
    MLOG_INFO("mirror", "[FEED-DBG] #%d len=%zu running=%d", fd+1, len, running_.load() ? 1 : 0);
  }
  bytes_received_.fetch_add(len);
  process_rtp_packet(data, len);
}

} // namespace gui
