#include "h264_decoder.hpp"

#include "../mirage_log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
}

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

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder() {
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

bool H264Decoder::init() {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
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

  // Try HW acceleration: D3D11VA → Vulkan → CPU
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

  for (const auto& opt : hw_options) {
    AVBufferRef* hw_device_ctx = nullptr;
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

  if (!hw_enabled_) {
    MLOG_INFO("h264", "No HW acceleration available, using CPU decode");
    codec_ctx_->thread_count = 2;
  }

  AVDictionary* opts = nullptr;
  if (!hw_enabled_) {
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "preset", "ultrafast", 0);
  }

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

    convert_frame_to_rgba(sw_frame);
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

    convert_frame_to_rgba(sw_frame);
    frames_out++;
    frames_decoded_++;
    av_frame_unref(frame_);
  }

  return frames_out;
}

void H264Decoder::convert_frame_to_rgba(AVFrame* frame) {
  if (!frame_callback_) return;

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

  // Reinitialize SwsContext if dimensions changed
  if (width != last_width_ || height != last_height_ || !sws_ctx_) {
    if (sws_ctx_) {
      sws_freeContext(sws_ctx_);
      sws_ctx_ = nullptr;
    }

    // Scale down large frames for faster conversion
    // Display doesn't need full resolution - halve if > 720p
    out_width_ = width;
    out_height_ = height;
    if (width > 1280 || height > 1280) {
      out_width_ = width / 2;
      out_height_ = height / 2;
      MLOG_INFO("h264", "Scaling output: %dx%d -> %dx%d", width, height, out_width_, out_height_);
    }

    sws_ctx_ = sws_getContext(
      width, height, (AVPixelFormat)frame->format,
      out_width_, out_height_, AV_PIX_FMT_RGBA,
      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) {
      MLOG_ERROR("h264", "Failed to create SwsContext for %dx%d fmt=%d -> %dx%d RGBA",
                 width, height, (int)frame->format, out_width_, out_height_);
      error_count_++;
      return;
    }

    // Allocate RGBA frame buffer
    av_frame_unref(frame_rgba_);
    frame_rgba_->format = AV_PIX_FMT_RGBA;
    frame_rgba_->width = out_width_;
    frame_rgba_->height = out_height_;
    if (av_frame_get_buffer(frame_rgba_, 32) < 0) {
      MLOG_ERROR("h264", "Failed to allocate RGBA frame buffer");
      error_count_++;
      // Reset frame_rgba_ to prevent use of partially initialized state
      av_frame_unref(frame_rgba_);
      sws_freeContext(sws_ctx_);
      sws_ctx_ = nullptr;
      last_width_ = 0;
      last_height_ = 0;
      return;
    }

    // Pre-allocate persistent RGBA buffer for padded frames
    size_t buffer_size = static_cast<size_t>(out_width_) * out_height_ * 4;
    if (rgba_buffer_.size() < buffer_size) {
      rgba_buffer_.resize(buffer_size);
    }

    last_width_ = width;
    last_height_ = height;
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
  if (frame_rgba_->linesize[0] == out_width_ * 4) {
    // No padding, can use directly
    // Callback receives pointer to internal buffer - valid only during callback
    frame_callback_(frame_rgba_->data[0], out_width_, out_height_, frame->pts);
  } else {
    // Has padding, copy to persistent buffer (avoids allocation per frame)
    uint8_t* dst = rgba_buffer_.data();
    uint8_t* src = frame_rgba_->data[0];
    for (int y = 0; y < out_height_; y++) {
      memcpy(dst, src, out_width_ * 4);
      dst += out_width_ * 4;
      src += frame_rgba_->linesize[0];
    }
    // Callback receives pointer to persistent buffer - valid only during callback
    frame_callback_(rgba_buffer_.data(), out_width_, out_height_, frame->pts);
  }
}

} // namespace gui
