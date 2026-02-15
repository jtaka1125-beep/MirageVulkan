// =============================================================================
// MirageSystem v2 - GUI Implementation Part 2
// =============================================================================
// Rendering: Layout calculation and frame management
// =============================================================================
#include "gui/gui_state.hpp"
#include "gui/gui_command.hpp"
#include "gui_application.hpp"
#include "adb_device_manager.hpp"
#include "hybrid_command_sender.hpp"

#include <imgui.h>
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"
#include <imgui_internal.h>

// For PNG decoding
#include "stb_image.h"
#include <shellapi.h>
#include "mirage_log.hpp"

namespace mirage::gui {

using namespace mirage::gui::state;

// =============================================================================
// Layout Calculation
// =============================================================================

GuiApplication::LayoutRects GuiApplication::calculateLayout() const {
    LayoutRects rects;

    float totalWidth = static_cast<float>(window_width_);
    float totalRatio = config_.left_ratio + config_.center_ratio + config_.right_ratio;

    // Ensure ratios are valid
    if (totalRatio <= 0) {
        totalRatio = 1.0f + 2.0f + 1.0f; // Default ratios
    }

    rects.left_w = totalWidth * config_.left_ratio / totalRatio;
    rects.center_w = totalWidth * config_.center_ratio / totalRatio;
    rects.right_w = totalWidth * config_.right_ratio / totalRatio;

    rects.left_x = 0;
    rects.center_x = rects.left_w;
    rects.right_x = rects.left_w + rects.center_w;

    rects.height = static_cast<float>(window_height_);

    return rects;
}

GuiApplication::SubGridLayout GuiApplication::calculateSubGrid(
        int device_count, float panel_w, float panel_h) const {

    SubGridLayout layout;

    // Adaptive grid based on device count
    // 2 devices: 1x1 (show only 1 in sub, other is main)
    // 3-5 devices: 2x2
    // 6-10 devices: 3x3

    if (device_count <= 2) {
        layout.cols = 1;
        layout.rows = 1;
    } else if (device_count <= 5) {
        layout.cols = 2;
        layout.rows = 2;
    } else {
        layout.cols = 3;
        layout.rows = 3;
    }

    float padding = static_cast<float>(config_.sub_grid_padding);
    layout.cell_w = (panel_w - padding * (layout.cols + 1)) / layout.cols;
    layout.cell_h = (panel_h - padding * (layout.rows + 1)) / layout.rows;

    return layout;
}

// =============================================================================
// Frame Rendering
// =============================================================================

void GuiApplication::beginFrame() {
    vulkanBeginFrame();
    if (!frame_valid_) return;  // Swapchain recreated, skip this frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void GuiApplication::render() {
    if (!frame_valid_) return;  // No valid Vulkan frame
    // Skip rendering during resize
    if (resizing_.load()) return;

    // Render panels
    renderLeftPanel();
    renderCenterPanel();
    renderRightPanel();

    // Render screenshot popup if active
    if (show_screenshot_popup_) {
        renderScreenshotPopup();
    }
}

void GuiApplication::endFrame() {
    if (!frame_valid_) return;  // No valid Vulkan frame
    if (resizing_.load()) return;

    ImGui::Render();

    vulkanEndFrame();
}

} // namespace mirage::gui
