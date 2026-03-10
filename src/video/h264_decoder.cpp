#include "h264_decoder.hpp"
#include <chrono>

#include "../mirage_log.hpp"
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
}

// Static mutex for D3D11VA readback serialization
// D3D11 immediate context is not thread-safe; only transfer_data needs serialization.
// avcodec_send_packet/receive_frame run fully parallel.
static std::mutex s_d3d11_readback_mutex;

// Static callback for hardware pixel format selection
static enum AVPixelFormat hw_get_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
  // The desired HW format is stored in ctx->opaque as an int
  auto desired = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
  for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
    if (*p == desired) {
      return desired;
    }
  }
  // Fallback to software format
  return pix_fmts[0];
}

namespace gui {

H264Decoder::H264Decoder() {
    instance_index_ = s_instance_count_.fetch_add(1);
}

H264Decoder::~H264Decoder() {
  s_instance_count_.fetch_sub(1);
  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }
  if (frame_rgba_) {
    av_frame_free(&frame_rgba_);
  }
  if (frame_) {
    av_frame_free(&frame_);
  }
  if (packet_) {
    av_packet_free(&packet_);
  }
  if (codec_ctx_) {
    avcodec_free_context(&codec_ctx_);
  }
  if (sw_frame_) { av_frame_free(&sw_frame_); }
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
}

bool H264Decoder::init(bool use_hevc) {
  is_hevc_ = use_hevc;

  const AVCodec* codec = avcodec_find_decoder(use_hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
  MLOG_INFO("h264", "Using %s decoder", use_hevc ? "H.265/HEVC" : "H.264");
  if (!codec) {
    return false;
  }

  codec_ctx_ = avcodec_alloc_context3(codec);
  if (!codec_ctx_) {
    return false;
  }

  // Enable error concealment for streaming
  codec_ctx_->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
  codec_ctx_->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;

  // Low latency settings
  codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  codec_ctx_->delay = 0;

  // Try HW acceleration: D3D11VA 竊・Vulkan 竊・CPU
  hw_enabled_ = false;
  hw_pix_fmt_ = -1;

  struct HwOption {
    AVHWDeviceType type;
    AVPixelFormat pix_fmt;
    const char* name;
  };
  const HwOption hw_options[] = {
    { AV_HWDEVICE_TYPE_D3D11VA, AV_PIX_FMT_D3D11,  "D3D11VA" },
    { AV_HWDEVICE_TYPE_VULKAN,  AV_PIX_FMT_VULKAN, "Vulkan" },
  };

  // AMD iGPU: D3D11VA readback is slower than CPU decode for 1200x1008 tiles.
  // Disable HW accel and use FF_THREAD_SLICE CPU decode for both tile instances.
  if (false) {  // HW disabled
    for (const auto& opt : hw_options) {
      AVBufferRef* hw_device_ctx = nullptr;
      // Create a new HW device for each instance.
      // flags=0: FFmpeg may reuse cached device internally, but each call
      // with D3D11VA on Windows creates separate command queues in practice.
      int hw_ret = av_hwdevice_ctx_create(&hw_device_ctx, opt.type, nullptr, nullptr, 0);
      if (hw_ret >= 0 && hw_device_ctx) {
        codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        codec_ctx_->get_format = hw_get_format;
        codec_ctx_->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(opt.pix_fmt));
        codec_ctx_->thread_count = 1;  // HW decode is single-thread
        hw_enabled_ = true;
        hw_pix_fmt_ = opt.pix_fmt;
        hw_device_ctx_ = hw_device_ctx;
        MLOG_INFO("h264", "%s hardware acceleration enabled", opt.name);
        break;
      } else {
        MLOG_INFO("h264", "%s not available (err=%d), trying next...", opt.name, hw_ret);
        if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
      }
    }
  }

  if (!hw_enabled_) {
    MLOG_INFO("h264", "No HW acceleration available, using CPU decode");
    codec_ctx_->thread_count = 0;  // 0 = auto (all logical CPUs; Ryzen 7840HS has 16)
    codec_ctx_->thread_type = FF_THREAD_SLICE;  // slice-level: low latency, no frame delay
  }

  AVDictionary* opts = nullptr;
  if (!hw_enabled_) {
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "preset", "ultrafast", 0);
  }

  // Relax error recognition for MediaTek HEVC VPS non-conformance.
  // c2.mtk.hevc.encoder emits VPS with base_layer flags that strict parsers reject.
  codec_ctx_->err_recognition = 0;

  if (avcodec_open2(codec_ctx_, codec, &opts) < 0) {
    av_dict_free(&opts);
    if (hw_device_ctx_) { av_buffer_unref(&hw_device_ctx_); hw_device_ctx_ = nullptr; }
    avcodec_free_context(&codec_ctx_);
    return false;
  }
  av_dict_free(&opts);

  frame_ = av_frame_alloc();
  frame_rgba_ = av_frame_alloc();
  sw_frame_ = av_frame_alloc();  // Pre-allocated for HW->CPU transfer
  packet_ = av_packet_alloc();

  if (!frame_ || !frame_rgba_ || !packet_) {
    // Cleanup partially allocated resources
    if (packet_) { av_packet_free(&packet_); }
    if (frame_rgba_) { av_frame_free(&frame_rgba_); }
    if (frame_) { av_frame_free(&frame_); }
    avcodec_free_context(&codec_ctx_);
    return false;
  }

  return true;
}

int H264Decoder::decode(const uint8_t* annexb_data, size_t len) {
  if (!codec_ctx_ || !annexb_data || len == 0) {
    return 0;
  }

  nals_fed_++;

  // Feed data to decoder
  packet_->data = const_cast<uint8_t*>(annexb_data);
  packet_->size = static_cast<int>(len);

  return decode_packet(packet_);
}

int H264Decoder::decode_packet(AVPacket* pkt) {
  int ret = avcodec_send_packet(codec_ctx_, pkt);
  if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
    send_packet_errors_++;
    error_count_++;
    // Log errors with throttling (per-instance counter now)
    if (send_packet_errors_ <= 20 || send_packet_errors_ % 100 == 0) {
      MLOG_ERROR("h264", "send_packet error: %d (total: %llu)", ret, (unsigned long long)send_packet_errors_);
    }
    return 0;
  }

  int frames_out = 0;
  while (ret >= 0) {
    ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }
    if (ret < 0) {
      receive_frame_errors_++;
      error_count_++;
      if (receive_frame_errors_ <= 10 || receive_frame_errors_ % 100 == 0) {
        MLOG_ERROR("h264", "receive_frame error: %d (total: %llu)", ret, (unsigned long long)receive_frame_errors_);
      }
      break;
    }

    // Got a frame! Log with per-instance counter
    if (frames_decoded_ < 5 || (frames_decoded_ + 1) % 100 == 0) {
      MLOG_INFO("h264", "DECODED FRAME #%llu: %dx%d", (unsigned long long)(frames_decoded_ + 1), frame_->width, frame_->height);
    }
    // If HW frame, transfer to CPU first
    // Serialize only the D3D11VA readback; decode runs fully parallel.
    AVFrame* sw_frame = frame_;
    if (hw_enabled_ && frame_->format == hw_pix_fmt_) {
      av_frame_unref(sw_frame_);
      if (av_hwframe_transfer_data(sw_frame_, frame_, 0) < 0) {
        MLOG_ERROR("h264", "Failed to transfer HW frame to CPU");
        av_frame_unref(frame_);
        continue;
      }
      if (frames_decoded_ < 3) {
        MLOG_INFO("h264", "HW transfer: fmt=%d w=%d h=%d", sw_frame_->format, sw_frame_->width, sw_frame_->height);
      }
      sw_frame = sw_frame_;
    }

    {
        auto _t0 = std::chrono::steady_clock::now();
        convert_frame_to_rgba(sw_frame);
        long long _us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - _t0).count();
        if (frames_decoded_ < 5 || frames_decoded_ % 100 == 0)
            MLOG_INFO("h264", "conv_rgba #%llu: %lldus",
                (unsigned long long)frames_decoded_, _us);
    }
    frames_out++;
    frames_decoded_++;
    av_frame_unref(frame_);
  }

  return frames_out;
}

int H264Decoder::flush() {
  if (!codec_ctx_) return 0;

  // Send nullptr packet to flush
  avcodec_send_packet(codec_ctx_, nullptr);

  int frames_out = 0;
  int ret = 0;
  while (ret >= 0) {
    ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }
    if (ret < 0) {
      break;
    }

    // If HW frame, transfer to CPU first
    AVFrame* sw_frame = frame_;
    if (hw_enabled_ && frame_->format == hw_pix_fmt_) {
      av_frame_unref(sw_frame_);
      if (av_hwframe_transfer_data(sw_frame_, frame_, 0) < 0) {
        MLOG_ERROR("h264", "Failed to transfer HW frame to CPU");
        av_frame_unref(frame_);
        continue;
      }
      if (frames_decoded_ < 3) {
        MLOG_INFO("h264", "HW transfer: fmt=%d w=%d h=%d", sw_frame_->format, sw_frame_->width, sw_frame_->height);
      }
      sw_frame = sw_frame_;
    }

    {
        auto _t0 = std::chrono::steady_clock::now();
        convert_frame_to_rgba(sw_frame);
        long long _us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - _t0).count();
        if (frames_decoded_ < 5 || frames_decoded_ % 100 == 0)
            MLOG_INFO("h264", "conv_rgba #%llu: %lldus",
                (unsigned long long)frames_decoded_, _us);
    }
    frames_out++;
    frames_decoded_++;
    av_frame_unref(frame_);
  }

  return frames_out;
}

void H264Decoder::convert_frame_to_rgba(AVFrame* frame) {
  if (!frame_callback_) {
    if (frames_decoded_ < 5) MLOG_WARN("h264", "frame_callback_ not set, skip frame");
    return;
  }

  int width = frame->width;
  int height = frame->height;

  // Sanity check dimensions
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
    MLOG_ERROR("h264", "Invalid frame dimensions: %dx%d", width, height);
    error_count_++;
    return;
  }

  // Check for memory overflow (width * height * 4 must fit in size_t)
  // Max allowed: 128MB to prevent excessive memory allocation
  constexpr size_t MAX_FRAME_BYTES = 128 * 1024 * 1024;
  size_t frame_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  if (frame_bytes > MAX_FRAME_BYTES) {
    MLOG_INFO("h264", "Frame too large: %dx%d (%zu bytes)", width, height, frame_bytes);
    error_count_++;
    return;
  }

  // Quick center-pixel YUV sample (first 5 frames + every 300)
  if (frames_decoded_ < 5 || frames_decoded_ % 300 == 0) {
    if (frame->data[0] && frame->data[1] && frame->data[2]) {
      int cy = frame->height/2, cx = frame->width/2;
      uint8_t yv = frame->data[0][cy * frame->linesize[0] + cx];
      uint8_t uv = frame->data[1][(cy/2) * frame->linesize[1] + cx/2];
      uint8_t vv = frame->data[2][(cy/2) * frame->linesize[2] + cx/2];
      MLOG_INFO("yuvsamp", "fd=%u Y=%d U=%d V=%d range=%d fmt=%d",
                frames_decoded_, yv, uv, vv, frame->color_range, frame->format);
    }
  }
  // Reinitialize SwsContext if dimensions, format, or color properties changed
  bool need_reinit = !sws_ctx_ ||
                     width != last_width_ ||
                     height != last_height_ ||
                     frame->format != last_format_ ||
                     frame->color_range != last_color_range_ ||
                     frame->colorspace != last_colorspace_;
  if (need_reinit) {
    if (sws_ctx_) {
      sws_freeContext(sws_ctx_);
      sws_ctx_ = nullptr;
    }

    // Keep native resolution for AI/macro accuracy (no scaling)
    out_width_ = width;
    out_height_ = height;

    sws_ctx_ = sws_getContext(
      width, height, (AVPixelFormat)frame->format,
      out_width_, out_height_, AV_PIX_FMT_RGBA,
      SWS_POINT, nullptr, nullptr, nullptr
    );

    // Set color space conversion for proper HEVC/H.264 color handling
    if (sws_ctx_) {
      // Determine colorspace from frame metadata, fallback to resolution-based heuristic
      int cs = SWS_CS_DEFAULT;
      if (frame->colorspace == AVCOL_SPC_BT709) {
        cs = SWS_CS_ITU709;
      } else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M) {
        cs = SWS_CS_ITU601;
      } else {
        // Fallback: HD uses BT.709, SD uses BT.601
        cs = (height >= 720) ? SWS_CS_ITU709 : SWS_CS_ITU601;
      }
      const int* coefs = sws_getCoefficients(cs);
      // MediaTek HEVC encoder reports MPEG range but actually outputs full range.
      // Force full range (srcRange=1) for all HEVC to fix washed-out colors.
      // For H.264, use frame's actual color_range.
      int srcRange = is_hevc_ ? 1 : ((frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0);
      sws_setColorspaceDetails(sws_ctx_, coefs, srcRange, coefs, 1, 0, 1 << 16, 1 << 16);
      if (frames_decoded_ < 5 || (frames_decoded_ % 500 == 0)) {
        MLOG_INFO("h264", "SwsContext: %dx%d fmt=%d cs=%d range=%d srcRange=%d (hevc=%d) -> RGBA",
                  width, height, (int)frame->format, frame->colorspace, frame->color_range, srcRange, is_hevc_ ? 1 : 0);
      }
    }

    if (!sws_ctx_) {
      MLOG_ERROR("h264", "Failed to create SwsContext for %dx%d fmt=%d -> %dx%d RGBA",
                 width, height, (int)frame->format, out_width_, out_height_);
      error_count_++;
      return;
    }

    // av_frame_get_buffer は AMD iGPU D3D11VA パスでハングする既知の問題。
    // rgba_buffer_ を直接使用して回避する。
    size_t buffer_size = static_cast<size_t>(out_width_) * out_height_ * 4;
    if (rgba_buffer_.size() < buffer_size) rgba_buffer_.resize(buffer_size);
    av_frame_unref(frame_rgba_);
    frame_rgba_->format      = AV_PIX_FMT_RGBA;
    frame_rgba_->width       = out_width_;
    frame_rgba_->height      = out_height_;
    frame_rgba_->data[0]     = rgba_buffer_.data();
    frame_rgba_->linesize[0] = out_width_ * 4;

    last_width_ = width;
    last_height_ = height;
    last_format_ = frame->format;
    last_color_range_ = frame->color_range;
    last_colorspace_ = frame->colorspace;
  }

  // Convert to RGBA
  int result = sws_scale(sws_ctx_,
    frame->data, frame->linesize,
    0, height,
    frame_rgba_->data, frame_rgba_->linesize
  );

  if (result != out_height_) {
    MLOG_INFO("h264", "sws_scale returned unexpected value: %d (expected %d)", result, out_height_);
    error_count_++;
    return;
  }

  // Call callback with RGBA data
  // Note: linesize might have padding, so we need to handle that
  if (frames_decoded_ < 5) MLOG_INFO("h264", "Calling frame_callback_ for frame %llu (%dx%d)", (unsigned long long)(frames_decoded_ + 1), out_width_, out_height_);
  frame_callback_(rgba_buffer_.data(), out_width_, out_height_, frame->pts);
  if (frames_decoded_ < 5) MLOG_INFO("h264", "frame_callback_ returned");
}

// Static member definition
std::atomic<int> H264Decoder::s_instance_count_{0};

} // namespace gui
