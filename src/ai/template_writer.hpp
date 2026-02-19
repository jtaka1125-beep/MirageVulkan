#pragma once
// =============================================================================
// テンプレートライター - PNG保存 (stb_image_write)
// MirageComplete/src/ai/template_writer から移行 (WIC → stb_image_write)
// =============================================================================
#include "ai/template_capture.hpp"
#include "result.hpp"
#include <string>

namespace mirage::ai {

// Gray8画像をPNGファイルに保存
mirage::Result<void> writeGray8Png(const std::string& path_utf8, const Gray8& img);

} // namespace mirage::ai
