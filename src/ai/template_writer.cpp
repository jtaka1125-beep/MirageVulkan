// =============================================================================
// テンプレートライター - stb_image_writeによるPNG保存
// =============================================================================
#include "ai/template_writer.hpp"
#include "mirage_log.hpp"

// STB_IMAGE_WRITE_IMPLEMENTATION is defined in gui_frame_capture_impl.cpp
#include "stb_image_write.h"

static constexpr const char* TAG = "TplWriter";

namespace mirage::ai {

mirage::Result<void> writeGray8Png(const std::string& path_utf8, const Gray8& img) {
    if (img.w <= 0 || img.h <= 0 || img.stride < img.w || img.pix.empty()) {
        return mirage::Err<void>("invalid image");
    }

    // stbi_write_png: comp=1 (grayscale), stride_in_bytes=img.stride
    int ret = stbi_write_png(
        path_utf8.c_str(),
        img.w, img.h,
        1,                      // channels (gray8)
        img.pix.data(),
        img.stride              // stride_in_bytes
    );

    if (ret == 0) {
        std::string err = "stbi_write_png失敗: " + path_utf8;
        MLOG_ERROR(TAG, "%s", err.c_str());
        return mirage::Err<void>(err);
    }

    MLOG_DEBUG(TAG, "PNG保存完了: %dx%d -> %s", img.w, img.h, path_utf8.c_str());
    return mirage::Ok();
}

} // namespace mirage::ai
