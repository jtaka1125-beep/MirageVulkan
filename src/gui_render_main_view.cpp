// =============================================================================
// MirageSystem v2 - GUI Implementation
// =============================================================================
// Rendering: Center panel, Right panel, Device view, Overlays, Status border
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
#include <atomic>

namespace mirage::gui {

using namespace mirage::gui::state;

// =============================================================================
// Center Panel (Main Device View)
// =============================================================================

void GuiApplication::renderCenterPanel() {
    auto layout = calculateLayout();

    ImGui::SetNextWindowPos(ImVec2(layout.center_x, 0));
    ImGui::SetNextWindowSize(ImVec2(layout.center_w, layout.height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("CenterPanel", nullptr, flags);

    // Copy device data under lock, then release before rendering
    std::string main_id;
    DeviceInfo main_device;
    bool has_main = false;

    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        main_id = main_device_id_;
        if (!main_id.empty()) {
            auto it = devices_.find(main_id);
            if (it != devices_.end()) {
                main_device = it->second;  // Copy
                has_main = true;
            }
        }
    }

    if (has_main) {
        // Header
        ImGui::Text(u8"\u30e1\u30a4\u30f3: %s", main_device.name.c_str());
        if (config_.show_fps) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                               "%.1f fps", main_device.fps);
        }
        if (config_.show_latency) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f),
                               "%.0f ms", main_device.latency_ms);
        }

        // Device view with space reserved for navigation bar
        const float nav_bar_h = 36.0f;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float view_x = ImGui::GetCursorScreenPos().x;
        float view_y = ImGui::GetCursorScreenPos().y;
        float view_h = avail.y - nav_bar_h - 4.0f;
        if (view_h < 100.0f) view_h = avail.y;  // Fallback if too small

        renderDeviceView(main_device, view_x, view_y, avail.x, view_h, true, false);

        // === Navigation Bar (Back / Home / Recents) ===
        if (view_h < avail.y) {  // Only show if space was reserved
            float btn_w = 70.0f;
            float bar_w = avail.x;
            float total_btn_w = btn_w * 3 + 8.0f * 2;
            float start_x = view_x + (bar_w - total_btn_w) / 2.0f;

            ImGui::SetCursorScreenPos(ImVec2(start_x, view_y + view_h + 2.0f));

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.35f, 0.45f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.20f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

            if (ImGui::Button(u8"< Back", ImVec2(btn_w, nav_bar_h - 4.0f))) {
                mirage::gui::command::sendKeyCommand(main_device.id, 4);  // KEYCODE_BACK
            }
            ImGui::SameLine(0, 8.0f);

            if (ImGui::Button(u8"o Home", ImVec2(btn_w, nav_bar_h - 4.0f))) {
                mirage::gui::command::sendKeyCommand(main_device.id, 3);  // KEYCODE_HOME
            }
            ImGui::SameLine(0, 8.0f);

            if (ImGui::Button(u8"= Task", ImVec2(btn_w, nav_bar_h - 4.0f))) {
                mirage::gui::command::sendKeyCommand(main_device.id, 187);  // KEYCODE_APP_SWITCH
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
        }
    } else {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize(u8"\u30c7\u30d0\u30a4\u30b9\u672a\u9078\u629e");
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) / 2,
            (avail.y - textSize.y) / 2
        ));
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), u8"\u30c7\u30d0\u30a4\u30b9\u672a\u9078\u629e");
    }

    ImGui::End();
}

// =============================================================================
// Right Panel (Sub Device Grid)
// =============================================================================

void GuiApplication::renderRightPanel() {
    auto layout = calculateLayout();

    ImGui::SetNextWindowPos(ImVec2(layout.right_x, 0));
    ImGui::SetNextWindowSize(ImVec2(layout.right_w, layout.height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("RightPanel", nullptr, flags);

    ImGui::Text(u8"\u30b5\u30d6\u30c7\u30d0\u30a4\u30b9");
    ImGui::Separator();

    // Copy device data under lock, then release before rendering
    std::vector<DeviceInfo> sub_device_list;
    size_t total_device_count = 0;

    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        total_device_count = devices_.size();

        for (const auto& id : device_order_) {
            if (id != main_device_id_) {
                auto it = devices_.find(id);
                if (it != devices_.end()) {
                    sub_device_list.push_back(it->second);  // Copy
                }
            }
        }
    }

    if (sub_device_list.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize(u8"\u30b5\u30d6\u30c7\u30d0\u30a4\u30b9\u306a\u3057");
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) / 2,
            (avail.y - textSize.y) / 2
        ));
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), u8"\u30b5\u30d6\u30c7\u30d0\u30a4\u30b9\u306a\u3057");
    } else {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        auto grid = calculateSubGrid(static_cast<int>(total_device_count), avail.x, avail.y);

        float padding = static_cast<float>(config_.sub_grid_padding);
        float start_x = ImGui::GetCursorScreenPos().x;
        float start_y = ImGui::GetCursorScreenPos().y;

        int idx = 0;
        int maxCells = grid.cols * grid.rows;

        for (int row = 0; row < grid.rows && idx < static_cast<int>(sub_device_list.size()); row++) {
            for (int col = 0; col < grid.cols && idx < static_cast<int>(sub_device_list.size()); col++) {
                if (idx >= maxCells) break;

                DeviceInfo& device = sub_device_list[idx];

                float cell_x = start_x + padding + col * (grid.cell_w + padding);
                float cell_y = start_y + padding + row * (grid.cell_h + padding);

                renderDeviceView(device, cell_x, cell_y, grid.cell_w, grid.cell_h, false, true);

                idx++;
            }
        }
    }

    ImGui::End();
}

// =============================================================================
// Device View Rendering
// =============================================================================

void GuiApplication::renderDeviceView(DeviceInfo& device,
                                        float x, float y, float w, float h,
                                        bool is_main, bool draw_border) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Validate dimensions to prevent division by zero
    if (w <= 0 || h <= 0) return;

    // Calculate aspect-ratio preserved dimensions
    float view_w = w;
    float view_h = h;
    float view_x = x;
    float view_y = y;

    // Clip all drawing to the allocated container rect
    draw_list->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);

    if (device.vk_texture_ds && device.texture_width > 0 && device.texture_height > 0) {
        float aspect = static_cast<float>(device.texture_width) / device.texture_height;
        float container_aspect = w / h;  // Safe: h > 0 guaranteed above

        if (aspect > container_aspect) {
            // Width-limited: fit to container width
            view_w = w;
            view_h = (aspect > 0) ? (w / aspect) : h;
            view_x = x;
            view_y = y + (h - view_h) / 2;
        } else {
            // Height-limited: fit to container height
            view_h = h;
            view_w = h * aspect;
            view_x = x + (w - view_w) / 2;
            view_y = y;
        }

        // Clamp to container bounds (defensive)
        if (view_x < x) view_x = x;
        if (view_y < y) view_y = y;
        if (view_x + view_w > x + w) view_w = x + w - view_x;
        if (view_y + view_h > y + h) view_h = y + h - view_y;

        // Store main view rect for input processing (thread-safe)
        if (is_main) {
            std::lock_guard<std::mutex> rect_lock(view_rect_mutex_);
            main_view_rect_.x = view_x;
            main_view_rect_.y = view_y;
            main_view_rect_.w = view_w;
            main_view_rect_.h = view_h;
            main_view_rect_.valid = true;
        }

        // Draw texture
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(device.vk_texture_ds),
            ImVec2(view_x, view_y),
            ImVec2(view_x + view_w, view_y + view_h)
        );




        // Draw overlays
        if (config_.show_match_boxes || config_.show_match_labels) {
            renderOverlays(device, view_x, view_y, view_w, view_h);
        }
    } else {
        // No texture - draw placeholder
        draw_list->AddRectFilled(
            ImVec2(view_x, view_y),
            ImVec2(view_x + view_w, view_y + view_h),
            IM_COL32(30, 30, 35, 255)
        );

        // Device name in center
        const char* text = device.name.c_str();
        ImVec2 textSize = ImGui::CalcTextSize(text);
        draw_list->AddText(
            ImVec2(view_x + (view_w - textSize.x) / 2, view_y + (view_h - textSize.y) / 2),
            IM_COL32(128, 128, 128, 255),
            text
        );

        // Mark main view as invalid when no texture
        if (is_main) {
            std::lock_guard<std::mutex> rect_lock(view_rect_mutex_);
            main_view_rect_.valid = false;
        }
    }

    draw_list->PopClipRect();

    // Status border
    if (draw_border) {
        renderStatusBorder(x, y, w, h, device.status, config_.sub_border_width);
    }

    // Interaction area (invisible button for click detection)
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    std::string btn_id = "##device_" + device.id;
    if (ImGui::InvisibleButton(btn_id.c_str(), ImVec2(w, h))) {
        // Single click - handled in mouse callbacks
    }

    // Note: Double-click handling moved to gui_input.cpp::onMouseDoubleClick()
    // to avoid duplicate event processing
}

// =============================================================================
// Overlay Rendering
// =============================================================================

void GuiApplication::renderOverlays(DeviceInfo& device,
                                     float view_x, float view_y,
                                     float view_w, float view_h) {
    if (device.texture_width <= 0 || device.texture_height <= 0) return;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    float scale_x = view_w / device.texture_width;
    float scale_y = view_h / device.texture_height;

    for (const auto& overlay : device.overlays) {
        float ox = view_x + overlay.x * scale_x;
        float oy = view_y + overlay.y * scale_y;
        float ow = overlay.w * scale_x;
        float oh = overlay.h * scale_y;

        // Box
        if (config_.show_match_boxes) {
            uint32_t col = overlay.color;
            if (col == 0) {
                // Default color based on score
                uint8_t g = static_cast<uint8_t>(overlay.score * 255);
                col = IM_COL32(0, g, 255 - g, 180);
            }

            draw_list->AddRect(
                ImVec2(ox, oy),
                ImVec2(ox + ow, oy + oh),
                col,
                0.0f,
                0,
                2.0f
            );
        }

        // Label
        if (config_.show_match_labels && !overlay.label.empty()) {
            ImVec2 textSize = ImGui::CalcTextSize(overlay.label.c_str());

            // Background
            draw_list->AddRectFilled(
                ImVec2(ox, oy - textSize.y - 2),
                ImVec2(ox + textSize.x + 4, oy),
                IM_COL32(0, 0, 0, 180)
            );

            // Text
            draw_list->AddText(
                ImVec2(ox + 2, oy - textSize.y - 1),
                IM_COL32(255, 255, 255, 255),
                overlay.label.c_str()
            );
        }
    }
}

// =============================================================================
// Status Border
// =============================================================================

void GuiApplication::renderStatusBorder(float x, float y, float w, float h,
                                          DeviceStatus status, int border_width) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    uint32_t col = getStatusColor(status);
    uint32_t imCol = IM_COL32(
        (col >> 0) & 0xFF,
        (col >> 8) & 0xFF,
        (col >> 16) & 0xFF,
        255
    );

    // Pulsing effect for active states
    float alpha = 1.0f;
    if (status == DeviceStatus::AndroidActive ||
        status == DeviceStatus::AIActive ||
        status == DeviceStatus::Stuck) {
        float t = static_cast<float>(getCurrentTimeMs() % 1000) / 1000.0f;
        alpha = 0.6f + 0.4f * std::sin(t * 3.14159f * 2);
        imCol = (imCol & 0x00FFFFFF) | (static_cast<uint32_t>(alpha * 255) << 24);
    }

    float bw = static_cast<float>(border_width);

    // Top
    draw_list->AddRectFilled(
        ImVec2(x, y),
        ImVec2(x + w, y + bw),
        imCol
    );
    // Bottom
    draw_list->AddRectFilled(
        ImVec2(x, y + h - bw),
        ImVec2(x + w, y + h),
        imCol
    );
    // Left
    draw_list->AddRectFilled(
        ImVec2(x, y),
        ImVec2(x + bw, y + h),
        imCol
    );
    // Right
    draw_list->AddRectFilled(
        ImVec2(x + w - bw, y),
        ImVec2(x + w, y + h),
        imCol
    );
}

} // namespace mirage::gui
