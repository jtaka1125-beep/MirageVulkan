// =============================================================================
// テンプレートキャプチャ - RGBAバッファからROI切出し → Gray8変換
// =============================================================================
#include "ai/template_capture.hpp"
#include "mirage_log.hpp"
#include <algorithm>
#include <cstring>

static constexpr const char* TAG = "TplCapture";

namespace mirage::ai {

static bool clampRoi(int fw, int fh, RoiRect& r) {
    int x0 = std::max(0, r.x);
    int y0 = std::max(0, r.y);
    int x1 = std::min(fw, r.x + r.w);
    int y1 = std::min(fh, r.y + r.h);
    int w = x1 - x0;
    int h = y1 - y0;
    if (w <= 0 || h <= 0) return false;
    r.x = x0;
    r.y = y0;
    r.w = w;
    r.h = h;
    return true;
}

// RGBA → Gray (luma近似: 0.299R + 0.587G + 0.114B)
static inline uint8_t toGray(uint8_t r, uint8_t g, uint8_t b) {
    int y = (77 * r + 150 * g + 29 * b + 128) >> 8;
    return (uint8_t)std::clamp(y, 0, 255);
}

mirage::Result<Gray8> captureTemplateGray8FromBuffer(
    const uint8_t* rgba_data,
    int frame_w, int frame_h,
    const RoiRect& roiIn,
    const CaptureConfig& cfg)
{
    if (!rgba_data) {
        return mirage::Err<Gray8>("rgba_data=null");
    }
    if (frame_w <= 0 || frame_h <= 0) {
        return mirage::Err<Gray8>("invalid frame size");
    }

    RoiRect roi = roiIn;
    if (cfg.allow_partial_clamp) {
        if (!clampRoi(frame_w, frame_h, roi)) {
            return mirage::Err<Gray8>("ROIクランプ後に無効");
        }
    } else {
        if (roi.x < 0 || roi.y < 0 || roi.x + roi.w > frame_w || roi.y + roi.h > frame_h) {
            return mirage::Err<Gray8>("ROI範囲外");
        }
        if (roi.w <= 0 || roi.h <= 0) {
            return mirage::Err<Gray8>("ROI w/h <= 0");
        }
    }

    Gray8 g;
    g.w = roi.w;
    g.h = roi.h;
    g.stride = roi.w;
    g.pix.resize((size_t)g.stride * g.h);

    const int src_stride = frame_w * 4;  // RGBA = 4 bytes per pixel

    for (int y = 0; y < g.h; y++) {
        const uint8_t* row = rgba_data + (size_t)(roi.y + y) * src_stride + (size_t)roi.x * 4;
        uint8_t* dst = &g.pix[(size_t)y * g.stride];
        for (int x = 0; x < g.w; x++) {
            uint8_t r = row[x * 4 + 0];
            uint8_t gg = row[x * 4 + 1];
            uint8_t b = row[x * 4 + 2];
            dst[x] = toGray(r, gg, b);
        }
    }

    MLOG_DEBUG(TAG, "キャプチャ完了: ROI(%d,%d,%d,%d) -> Gray8 %dx%d",
               roi.x, roi.y, roi.w, roi.h, g.w, g.h);

    return g;
}

} // namespace mirage::ai
