// =============================================================================
// MirageSystem v2 - GUI Implementation
// =============================================================================
// Rendering: Screenshot capture and popup dialog
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
// Screenshot Capture
// =============================================================================

void GuiApplication::captureScreenshot(const std::string& device_id) {
    if (!adb_manager_) {
        logError(u8"\u30b9\u30af\u30ea\u30fc\u30f3\u30b7\u30e7\u30c3\u30c8: ADB\u30de\u30cd\u30fc\u30b8\u30e3\u672a\u8a2d\u5b9a");
        return;
    }

    logInfo(u8"\u30b9\u30af\u30ea\u30fc\u30f3\u30b7\u30e7\u30c3\u30c8\u53d6\u5f97\u4e2d: " + device_id);

    // Get screenshot data from ADB
    screenshot_data_ = adb_manager_->takeScreenshot(device_id);
    screenshot_device_id_ = device_id;

    if (screenshot_data_.empty()) {
        logError(u8"\u30b9\u30af\u30ea\u30fc\u30f3\u30b7\u30e7\u30c3\u30c8\u53d6\u5f97\u5931\u6557");
        return;
    }

    logInfo(u8"\u30b9\u30af\u30ea\u30fc\u30f3\u30b7\u30e7\u30c3\u30c8\u53d6\u5f97\u6210\u529f: " + std::to_string(screenshot_data_.size()) + " bytes");

    // Decode PNG to RGBA
    int width, height, channels;
    unsigned char* img = stbi_load_from_memory(
        screenshot_data_.data(),
        static_cast<int>(screenshot_data_.size()),
        &width, &height, &channels, 4);

    if (!img) {
        logError(u8"PNG \u30c7\u30b3\u30fc\u30c9\u5931\u6557");
        screenshot_data_.clear();
        return;
    }

    // Create Vulkan texture for screenshot
    screenshot_vk_texture_ = std::make_unique<mirage::vk::VulkanTexture>();
    if (!screenshot_vk_texture_->create(*vk_context_, vk_descriptor_pool_, width, height)) {
        logError("Failed to create Vulkan texture for screenshot");
        stbi_image_free(img);
        screenshot_data_.clear();
        screenshot_vk_texture_.reset();
        return;
    }
    screenshot_vk_texture_->update(vk_command_pool_, vk_context_->graphicsQueue(), img, width, height);
    screenshot_vk_ds_ = screenshot_vk_texture_->imguiDescriptorSet();
    stbi_image_free(img);

    screenshot_width_ = width;
    screenshot_height_ = height;
    show_screenshot_popup_ = true;

    // Clear PNG data after texture creation to reduce memory usage
    // (PNG data is no longer needed once texture is on GPU)
    screenshot_data_.clear();
    screenshot_data_.shrink_to_fit();

    logInfo(u8"\u30b9\u30af\u30ea\u30fc\u30f3\u30b7\u30e7\u30c3\u30c8\u8868\u793a: " + std::to_string(width) + "x" + std::to_string(height));
}

// =============================================================================
// Screenshot Popup
// =============================================================================

void GuiApplication::renderScreenshotPopup() {
    if (!screenshot_vk_ds_) {
        show_screenshot_popup_ = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(
        static_cast<float>(window_width_) * 0.8f,
        static_cast<float>(window_height_) * 0.9f
    ), ImGuiCond_Appearing);

    ImGui::SetNextWindowPos(ImVec2(
        static_cast<float>(window_width_) * 0.1f,
        static_cast<float>(window_height_) * 0.05f
    ), ImGuiCond_Appearing);

    // Popup should appear on top - set focus
    ImGui::SetNextWindowFocus();

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;

    std::string title = u8"\u30b9\u30af\u30ea\u30fc\u30f3\u30b7\u30e7\u30c3\u30c8 - " + screenshot_device_id_ + "###Screenshot";
    if (ImGui::Begin(title.c_str(), &show_screenshot_popup_, flags)) {
        // Close button and info
        if (ImGui::Button(u8"\u9589\u3058\u308b")) {
            show_screenshot_popup_ = false;
            screenshot_vk_texture_.reset(); screenshot_vk_ds_ = VK_NULL_HANDLE;
            screenshot_data_.clear();
        }
        ImGui::SameLine();
        ImGui::Text(u8"\u30b5\u30a4\u30ba: %dx%d", screenshot_width_, screenshot_height_);

        ImGui::Separator();

        // Calculate image size to fit in window with aspect ratio
        ImVec2 avail = ImGui::GetContentRegionAvail();

        // Validate dimensions to prevent division by zero
        if (screenshot_width_ <= 0 || screenshot_height_ <= 0 ||
            avail.x <= 0 || avail.y <= 0) {
            ImGui::Text(u8"\u753b\u50cf\u3092\u8aad\u307f\u8fbc\u307f\u4e2d...");
        } else {
            float aspect = static_cast<float>(screenshot_width_) / screenshot_height_;
            float container_aspect = avail.x / avail.y;

            float img_w, img_h;
            if (aspect > container_aspect) {
                img_w = avail.x;
                img_h = (aspect > 0) ? (avail.x / aspect) : avail.y;
            } else {
                img_h = avail.y;
                img_w = avail.y * aspect;
            }

            // Center the image
            float offset_x = (avail.x - img_w) / 2;
            float offset_y = (avail.y - img_h) / 2;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset_y);

            // Display image
            if (screenshot_vk_ds_) {
                ImGui::Image(reinterpret_cast<ImTextureID>(screenshot_vk_ds_), ImVec2(img_w, img_h));
            }
        }
    }
    ImGui::End();

    // If popup closed, clean up
    if (!show_screenshot_popup_) {
        screenshot_vk_texture_.reset(); screenshot_vk_ds_ = VK_NULL_HANDLE;
        screenshot_data_.clear();
    }
}

} // namespace mirage::gui
