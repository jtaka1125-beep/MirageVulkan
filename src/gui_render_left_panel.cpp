// =============================================================================
// MirageSystem v2 - GUI Implementation
// =============================================================================
// Rendering: Left panel (Controls, Learning, Logs)
// =============================================================================
#include "gui/gui_state.hpp"
#include "gui/gui_command.hpp"
#include "gui_application.hpp"
#include "adb_device_manager.hpp"
#include "hybrid_command_sender.hpp"
#include "auto_setup.hpp"

#include <imgui.h>
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"
#include <imgui_internal.h>

// For PNG decoding
#include "stb_image.h"
#include <shellapi.h>
#include "mirage_log.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <mutex>

namespace mirage::gui {

// Track async operations for AutoSetup
static std::vector<std::future<void>> s_auto_setup_ops;
static std::mutex s_auto_setup_mutex;
static std::atomic<bool> s_auto_setup_running{false};

static void trackAutoSetupAsync(std::future<void> fut) {
    std::lock_guard<std::mutex> lock(s_auto_setup_mutex);
    // Clean completed futures
    s_auto_setup_ops.erase(
        std::remove_if(s_auto_setup_ops.begin(), s_auto_setup_ops.end(),
            [](const std::future<void>& f) {
                return f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
            }),
        s_auto_setup_ops.end());
    s_auto_setup_ops.push_back(std::move(fut));
    // Hard limit to prevent unbounded growth
    while (s_auto_setup_ops.size() > 10) {
        s_auto_setup_ops.erase(s_auto_setup_ops.begin());
    }
}

using namespace mirage::gui::state;

// =============================================================================
// Left Panel (Controls, Learning, Logs)
// =============================================================================

void GuiApplication::renderLeftPanel() {
    auto layout = calculateLayout();

    ImGui::SetNextWindowPos(ImVec2(layout.left_x, 0));
    ImGui::SetNextWindowSize(ImVec2(layout.left_w, layout.height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("LeftPanel", nullptr, flags);

    // === Device Selection ===
    ImGui::Text(u8"デバイス");
    ImGui::Separator();

    // Copy device data under lock, then render without lock
    std::vector<std::pair<DeviceInfo, bool>> device_list_copy;
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (const auto& id : device_order_) {
            auto it = devices_.find(id);
            if (it == devices_.end()) continue;
            device_list_copy.emplace_back(it->second, id == main_device_id_);
        }
    }

    {
        for (const auto& [device, is_main] : device_list_copy) {

            // Status color indicator
            uint32_t col = getStatusColor(device.status);
            ImVec4 colVec(
                ((col >> 0) & 0xFF) / 255.0f,
                ((col >> 8) & 0xFF) / 255.0f,
                ((col >> 16) & 0xFF) / 255.0f,
                1.0f
            );

            ImGui::PushStyleColor(ImGuiCol_Text, colVec);
            ImGui::Bullet();
            ImGui::PopStyleColor();

            ImGui::SameLine();

            if (is_main) {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "[MAIN]");
                ImGui::SameLine();
            }

            if (ImGui::Selectable(device.name.c_str(), is_main)) {
                setMainDevice(device.id);
            }

            // Show AOA version if checked
            if (device.aoa_version >= 0) {
                ImGui::SameLine();
                ImGui::TextDisabled(u8"AOA:v%d%s", device.aoa_version,
                    device.aoa_version >= 2 ? "(HID)" : "");
            }

            // Tooltip with stats
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(u8"ID: %s", device.id.c_str());
                ImGui::Text(u8"状態: %s", getStatusText(device.status).c_str());
                ImGui::Text(u8"FPS: %.1f", device.fps);
                ImGui::Text(u8"遅延: %.1f ms", device.latency_ms);
                ImGui::Text(u8"帯域: %.2f Mbps", device.bandwidth_mbps);
                if (device.aoa_version >= 0) {
                    ImGui::Text(u8"AOA: v%d%s", device.aoa_version,
                        device.aoa_version >= 2 ? " (HID対応)" : "");
                }
                ImGui::EndTooltip();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === Learning Mode ===
    ImGui::Text(u8"学習モード");
    ImGui::Separator();

    bool learning = learning_session_.active;
    if (ImGui::Checkbox(u8"学習を有効化", &learning)) {
        if (learning) {
            startLearningSession("Session_" + std::to_string(getCurrentTimeMs()));
        } else {
            stopLearningSession();
        }
    }

    if (learning_session_.active) {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), u8"クリック記録中...");
        ImGui::Text(u8"収集数: %zu", learning_session_.collected_clicks.size());

        if (ImGui::Button(u8"データ出力")) {
            exportLearningData();

        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === Actions ===
    ImGui::Text(u8"\u64cd\u4f5c");
    ImGui::Separator();

    if (ImGui::Button(u8"\u30b9\u30af\u30ea\u30fc\u30f3\u30b7\u30e7\u30c3\u30c8", ImVec2(-1, 0))) {
        // Capture from main device, fallback to first available
        if (adb_manager_) {
            std::string target;
            {
                std::lock_guard<std::mutex> lock(devices_mutex_);
                target = main_device_id_;
            }
            if (target.empty()) {
                auto devices = adb_manager_->getUniqueDevices();
                if (!devices.empty()) target = devices[0].preferred_adb_id;
            }
            if (!target.empty()) {
                captureScreenshot(target);
            } else {
                logWarning(u8"ADB\u30c7\u30d0\u30a4\u30b9\u306a\u3057");
            }
        } else {
            logWarning(u8"ADB\u30de\u30cd\u30fc\u30b8\u30e3\u672a\u8a2d\u5b9a");
        }
    }

    // Auto Setup button (disabled while running)
    if (s_auto_setup_running) {
        ImGui::BeginDisabled();
        ImGui::Button(u8"自動セットアップ実行中...", ImVec2(-1, 0));
        ImGui::EndDisabled();
    } else if (ImGui::Button(u8"Auto Setup (Accessibility)", ImVec2(-1, 0))) {
        if (adb_manager_) {
            auto devices = adb_manager_->getUniqueDevices();
            if (!devices.empty()) {
                std::string device_id = devices[0].preferred_adb_id;
                auto* mgr = adb_manager_;
                s_auto_setup_running = true;
                logInfo(u8"自動セットアップ開始: " + device_id, "AutoSetup");

                trackAutoSetupAsync(std::async(std::launch::async, [mgr, device_id]() {
                    mirage::AutoSetup setup;

                    setup.set_adb_executor([mgr, device_id](const std::string& cmd) -> std::string {
                        return mgr->adbCommand(device_id, cmd);
                    });

                    setup.set_progress_callback([](const std::string& step, int progress) {
                        auto gui = g_gui;
                        if (gui) {
                            gui->logInfo("[AutoSetup] " + step + " (" + std::to_string(progress) + "%)", "AutoSetup");
                        }
                    });

                    auto result = setup.run(true);

                    auto gui = g_gui;
                    if (gui) {
                        if (result.success) {
                            gui->logInfo(u8"自動セットアップ完了: " + result.summary(), "AutoSetup");
                        } else {
                            gui->logError(u8"自動セットアップ失敗: " + result.summary(), "AutoSetup");
                        }
                    }
                    s_auto_setup_running = false;
                }));
            } else {
                logWarning(u8"ADBデバイスなし");
            }
        } else {
            logWarning(u8"ADBマネージャ未設定");
        }
    }

    ImGui::Spacing();

    // === Driver Setup ===
    ImGui::Text(u8"ドライバ設定");
    ImGui::Separator();

    if (ImGui::Button(u8"WinUSB \u30c9\u30e9\u30a4\u30d0 \u30a4\u30f3\u30b9\u30c8\u30fc\u30eb", ImVec2(-1, 0))) {
        // Launch install_android_winusb.py with admin elevation via ShellExecute
        logInfo(u8"WinUSB \u30a4\u30f3\u30b9\u30c8\u30fc\u30e9\u30fc\u8d77\u52d5\u4e2d...");

        char exe_path[MAX_PATH] = {0};
        DWORD path_len = GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        if (path_len == 0 || path_len >= MAX_PATH) {
            MLOG_ERROR("render", "GetModuleFileNameA failed");
        } else {
            std::string exe_dir(exe_path);
            size_t last_slash = exe_dir.find_last_of("\\\\/");
            if (last_slash != std::string::npos) exe_dir = exe_dir.substr(0, last_slash);

            // Search for install_android_winusb.py
            std::vector<std::string> script_paths = {
                exe_dir + "\\..\\install_android_winusb.py",
                exe_dir + "\\install_android_winusb.py",
                exe_dir + "\\..\\driver_installer\\tools\\install_android_winusb.py",
                "C:\\MirageWork\\MirageComplete\\install_android_winusb.py",
                "C:\\MirageWork\\MirageComplete\\driver_installer\\tools\\install_android_winusb.py",
            };
            const char* mirage_home = std::getenv("MIRAGE_HOME");
            if (mirage_home) {
                script_paths.insert(script_paths.begin(),
                    std::string(mirage_home) + "\\install_android_winusb.py");
            }

            std::string script;
            for (const auto& sp : script_paths) {
                if (GetFileAttributesA(sp.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    script = sp;
                    break;
                }
            }

            if (script.empty()) {
                logError(u8"install_android_winusb.py \u304c\u898b\u3064\u304b\u308a\u307e\u305b\u3093");
                MLOG_WARN("render", "WinUSB installer script not found in any search path");
            } else {
                // Use ShellExecute with "runas" for admin elevation (UAC)
                std::string params = "\"" + script + "\"";
                HINSTANCE result = ShellExecuteA(nullptr, "runas", "python",
                                                  params.c_str(), nullptr, SW_SHOW);
                if (reinterpret_cast<intptr_t>(result) > 32) {
                    logInfo(u8"WinUSB \u30a4\u30f3\u30b9\u30c8\u30fc\u30e9\u30fc\u8d77\u52d5\u5b8c\u4e86");
                } else {
                    logError(u8"WinUSB \u30a4\u30f3\u30b9\u30c8\u30fc\u30e9\u30fc\u8d77\u52d5\u5931\u6557 (\u7ba1\u7406\u8005\u6a29\u9650\u304c\u5fc5\u8981)");
                }
            }
        }
    }

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(u8"WinUSB AOA\u30c9\u30e9\u30a4\u30d0\u306e\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb/\u7ba1\u7406");
        ImGui::Text(u8"\u203b \u7ba1\u7406\u8005\u6a29\u9650\u304c\u5fc5\u8981\u3067\u3059");
        ImGui::EndTooltip();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === Statistics ===
    ImGui::Text(u8"統計");
    ImGui::Separator();

    {
        std::lock_guard<std::mutex> lock(devices_mutex_);

        int connected = 0;
        float total_bandwidth = 0;
        for (const auto& [id, device] : devices_) {
            if (device.status != DeviceStatus::Disconnected) {
                connected++;
                total_bandwidth += device.bandwidth_mbps;
            }
        }

        ImGui::Text(u8"接続数: %d / %zu", connected, devices_.size());
        ImGui::Text(u8"合計帯域: %.1f Mbps", total_bandwidth);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === Touch Input Mode ===
    if (g_hybrid_cmd) {
        ImGui::Text(u8"タッチ入力: %s", g_hybrid_cmd->get_touch_mode_str().c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === Test Commands ===
    ImGui::Text(u8"テストコマンド");
    ImGui::Separator();

    if (ImGui::Button(u8"全デバイスに画面中央タップ", ImVec2(-1, 0))) {
        if (g_hybrid_cmd) {
            int count = g_hybrid_cmd->send_tap_all(
                static_cast<int>(layout_constants::TEST_TAP_X),
                static_cast<int>(layout_constants::TEST_TAP_Y),
                layout_constants::DEFAULT_SCREEN_W,
                layout_constants::DEFAULT_SCREEN_H);
            logInfo(u8"中央タップ送信: " + std::to_string(count) + u8"台");
        }
    }

    if (ImGui::Button(u8"全デバイスにホームキー", ImVec2(-1, 0))) {
        if (g_hybrid_cmd) {
            int count = g_hybrid_cmd->send_key_all(3);  // KEYCODE_HOME = 3
            logInfo(u8"ホームキー送信: " + std::to_string(count) + u8"台");
        }
    }

    if (ImGui::Button(u8"全デバイスに長押し (中央)", ImVec2(-1, 0))) {
        if (g_hybrid_cmd) {
            auto ids = g_hybrid_cmd->get_device_ids();
            for (const auto& id : ids) {
                g_hybrid_cmd->send_long_press(id,
                    static_cast<int>(layout_constants::TEST_TAP_X),
                    static_cast<int>(layout_constants::TEST_TAP_Y),
                    layout_constants::DEFAULT_SCREEN_W,
                    layout_constants::DEFAULT_SCREEN_H, 500);
            }
            logInfo(u8"長押し送信: " + std::to_string(ids.size()) + u8"台");
        }
    }

    if (ImGui::Button(u8"全デバイスにピンチアウト", ImVec2(-1, 0))) {
        if (g_hybrid_cmd) {
            auto ids = g_hybrid_cmd->get_device_ids();
            for (const auto& id : ids) {
                g_hybrid_cmd->send_pinch(id,
                    layout_constants::DEFAULT_SCREEN_W / 2,
                    layout_constants::DEFAULT_SCREEN_H / 2,
                    100, 400,
                    layout_constants::DEFAULT_SCREEN_W,
                    layout_constants::DEFAULT_SCREEN_H, 400);
            }
            logInfo(u8"ピンチアウト送信: " + std::to_string(ids.size()) + u8"台");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === Log ===
    ImGui::Text(u8"ログ");
    ImGui::Separator();

    // Log filter
    static int log_level_filter = 0;
    static const char* log_level_items[] = { u8"全て", u8"情報以上", u8"警告以上", u8"エラー" };
    ImGui::Combo(u8"レベル", &log_level_filter, log_level_items, IM_ARRAYSIZE(log_level_items));

    // Log area
    float logHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("LogScroll", ImVec2(0, logHeight), true);

    {
        std::lock_guard<std::mutex> lock(logs_mutex_);

        for (const auto& entry : logs_) {
            // Filter by level
            if (log_level_filter == 1 && entry.level == LogEntry::Level::Debug) continue;
            if (log_level_filter == 2 &&
                (entry.level == LogEntry::Level::Debug ||
                 entry.level == LogEntry::Level::Info)) continue;
            if (log_level_filter == 3 && entry.level != LogEntry::Level::Error) continue;

            ImVec4 col(
                ((entry.getColor() >> 0) & 0xFF) / 255.0f,
                ((entry.getColor() >> 8) & 0xFF) / 255.0f,
                ((entry.getColor() >> 16) & 0xFF) / 255.0f,
                1.0f
            );

            ImGui::TextColored(col, "[%s] %s",
                               entry.source.c_str(),
                               entry.message.c_str());
        }

        // Auto scroll - always scroll to bottom when enabled
        if (config_.auto_scroll_log) {
            ImGui::SetScrollHereY(1.0f);
        }
    }

    ImGui::EndChild();

    ImGui::End();
}

} // namespace mirage::gui
