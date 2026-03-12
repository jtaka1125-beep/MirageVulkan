// =============================================================================
// MirageSystem v2 - Frame Capture Implementation
// =============================================================================
// stb_image_write symbols are provided by stb_image_impl.cpp (STB_IMAGE_WRITE_IMPLEMENTATION).
// This file provides mirageGuiSavePng() called from GuiApplication::updateDeviceFrame.
// =============================================================================
#include "mirage_log.hpp"
#include <string>
#include <cstdint>

#include "stb_image_write.h"

namespace mirage::gui {

/// Save RGBA buffer as PNG. Called from GuiApplication::updateDeviceFrame.
bool mirageGuiSavePng(const char* path, int w, int h, const uint8_t* rgba) {
    int ok = stbi_write_png(path, w, h, 4, rgba, w * 4);
    if (ok) {
        MLOG_INFO("capture", "Frame saved: %s (%dx%d)", path, w, h);
    } else {
        MLOG_ERROR("capture", "Failed to save frame: %s", path);
    }
    return ok != 0;
}

} // namespace mirage::gui
