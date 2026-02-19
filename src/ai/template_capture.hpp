#pragma once
// =============================================================================
// テンプレートキャプチャ - VulkanImageからテンプレート切出し
// MirageComplete/src/ai/template_capture から移行 (D3D11 → Vulkan)
// =============================================================================
#include "result.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace mirage::vk {
class VulkanImage;
}

namespace mirage::ai {

struct RoiRect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Gray8 {
    int w = 0;
    int h = 0;
    int stride = 0;
    std::vector<uint8_t> pix;
};

struct CaptureConfig {
    bool allow_partial_clamp = true;
};

// RGBAフレームバッファ（CPU側）からROI切出し → Gray8変換
mirage::Result<Gray8> captureTemplateGray8FromBuffer(
    const uint8_t* rgba_data,
    int frame_w, int frame_h,
    const RoiRect& roi,
    const CaptureConfig& cfg = {});

} // namespace mirage::ai
