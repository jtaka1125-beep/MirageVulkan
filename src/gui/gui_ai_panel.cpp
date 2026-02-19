// =============================================================================
// MirageSystem v2 GUI - AI Engine Control Panel
// =============================================================================
// AIエンジンの制御・監視ImGuiパネル実装。
// gui_device_control.cppのパターンに準拠。
// =============================================================================

#include "gui_ai_panel.hpp"

#ifdef USE_AI

#include "gui_state.hpp"
#include "imgui.h"

#include <string>
#include <vector>
#include <cstdio>

namespace mirage::gui::ai_panel {

using namespace mirage::gui::state;

// VisionState定数（vision_decision_engine.hppのenumと同値）
static constexpr int VS_IDLE           = 0;
static constexpr int VS_DETECTED       = 1;
static constexpr int VS_CONFIRMED      = 2;
static constexpr int VS_COOLDOWN       = 3;
static constexpr int VS_ERROR_RECOVERY = 4;

// VisionState名称
static const char* visionStateName(int state) {
    switch (state) {
        case VS_IDLE:           return "IDLE";
        case VS_DETECTED:       return "DETECTED";
        case VS_CONFIRMED:      return "CONFIRMED";
        case VS_COOLDOWN:       return "COOLDOWN";
        case VS_ERROR_RECOVERY: return "ERROR_RECOVERY";
        default:                return "UNKNOWN";
    }
}

// VisionState色
static ImVec4 visionStateColor(int state) {
    switch (state) {
        case VS_IDLE:           return ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // グレー
        case VS_DETECTED:       return ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // 黄
        case VS_CONFIRMED:      return ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // 緑
        case VS_COOLDOWN:       return ImVec4(0.3f, 0.5f, 1.0f, 1.0f); // 青
        case VS_ERROR_RECOVERY: return ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // 赤
        default:                return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

// パネル状態
static float s_overlay_threshold = 0.80f;
static bool s_overlay_enabled = true;

// =============================================================================
// セクション1: AIエンジン制御
// =============================================================================

static void renderEngineControl() {
    if (!g_ai_engine) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "AI Engine: 未初期化");
        return;
    }

    // ON/OFF トグル
    bool enabled = g_ai_enabled.load();
    if (ImGui::Checkbox("AI Engine##toggle", &enabled)) {
        g_ai_engine->setEnabled(enabled);
        g_ai_enabled.store(enabled);
    }

    ImGui::SameLine(200);

    // リセットボタン
    if (ImGui::SmallButton("Reset##ai")) {
        g_ai_engine->reset();
        g_ai_engine->resetStats();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("統計とVisionDecisionEngine状態をリセット");
    }

    // 統計表示
    auto stats = g_ai_engine->getStats();

    ImGui::Text("Processed: %llu frames", (unsigned long long)stats.frames_processed);
    ImGui::SameLine(200);
    ImGui::Text("Actions: %llu", (unsigned long long)stats.actions_executed);

    // 処理時間（色分け）
    ImVec4 time_color;
    if (stats.avg_process_time_ms < 10.0) {
        time_color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // 緑 <10ms
    } else if (stats.avg_process_time_ms < 30.0) {
        time_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // 黄 <30ms
    } else {
        time_color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);  // 赤 >=30ms
    }
    ImGui::TextColored(time_color, "Avg: %.1f ms", stats.avg_process_time_ms);
    ImGui::SameLine(200);
    ImGui::Text("Templates: %d", stats.templates_loaded);

    ImGui::Text("Idle frames: %d", stats.idle_frames);
}

// =============================================================================
// セクション2: VisionDecisionEngine状態表示
// =============================================================================

static void renderVisionStates() {
    if (!g_ai_engine) return;

    auto states = g_ai_engine->getAllDeviceVisionStates();

    if (states.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "全デバイス IDLE");
    } else {
        for (const auto& [device_id, state] : states) {
            ImGui::TextColored(visionStateColor(state), "[%s]",
                               visionStateName(state));
            ImGui::SameLine();
            ImGui::Text("%s", device_id.c_str());
            ImGui::SameLine(220);
            std::string reset_label = "Reset##vde_" + device_id;
            if (ImGui::SmallButton(reset_label.c_str())) {
                g_ai_engine->resetDeviceVision(device_id);
            }
        }
    }

    // Reset All
    if (ImGui::SmallButton("Reset All##vde")) {
        g_ai_engine->resetAllVision();
    }

    // VDE設定値表示
    auto vde_cfg = g_ai_engine->getVDEConfig();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
        "confirm=%d  cooldown=%dms  debounce=%dms",
        vde_cfg.confirm_count, vde_cfg.cooldown_ms, vde_cfg.debounce_window_ms);
}

// =============================================================================
// セクション3: テンプレートマッチ一覧
// =============================================================================

static void renderMatchResults() {
    if (!g_ai_engine) return;

    auto matches = g_ai_engine->getLastMatches();

    if (matches.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "マッチなし");
        return;
    }

    ImGui::Text("%d matches:", (int)matches.size());

    for (const auto& m : matches) {
        // スコアに応じた色
        bool above_threshold = (m.score >= s_overlay_threshold);
        ImVec4 color = above_threshold
            ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)   // 緑: threshold以上
            : ImVec4(0.8f, 0.8f, 0.8f, 1.0f);   // グレー: 未満

        // テンプレート名
        ImGui::TextColored(color, "%s", m.template_id.c_str());

        // スコアプログレスバー
        ImGui::SameLine(160);
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%.1f%%", m.score * 100.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
            above_threshold ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                            : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::ProgressBar(m.score, ImVec2(80, 14), overlay);
        ImGui::PopStyleColor();

        // 座標
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "(%d,%d)", m.center_x, m.center_y);
    }
}

// =============================================================================
// セクション4: LearningMode制御
// =============================================================================

static void renderLearningMode() {
    if (!g_learning_mode) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "LearningMode: 未初期化");
        return;
    }

    bool running = g_learning_mode->is_running();

    // Start/Stop トグル
    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("Stop Learning", ImVec2(120, 25))) {
            g_learning_mode->stop();
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.5f, 0.1f, 1.0f));
        if (ImGui::Button("Start Learning", ImVec2(120, 25))) {
            g_learning_mode->start();
        }
        ImGui::PopStyleColor(3);
    }

    ImGui::SameLine();
    ImGui::TextColored(
        running ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
        running ? "Running" : "Stopped");

    // テンプレートキャプチャ（将来実装プレースホルダー）
    ImGui::BeginDisabled(true);
    ImGui::Button("Capture Template", ImVec2(120, 25));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("テンプレートキャプチャ（将来実装）");
    }
}

// =============================================================================
// セクション5: マッチオーバーレイ設定
// =============================================================================

static void renderOverlaySettings() {
    ImGui::Checkbox("Overlay##match_overlay", &s_overlay_enabled);

    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("Threshold##overlay", &s_overlay_threshold, 0.5f, 1.0f, "%.2f");
}

// =============================================================================
// メインパネル描画
// =============================================================================

void renderAIPanel() {
    if (!g_ai_engine) return;

    static bool first_frame = true;
    ImGuiIO& io = ImGui::GetIO();
    float panel_width = 340.0f;

    if (first_frame) {
        // Device Controlパネルの下に配置
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panel_width - 10, 200));
        ImGui::SetNextWindowCollapsed(true);  // 初期状態は折りたたみ
        first_frame = false;
    }

    if (!ImGui::Begin("AI Engine", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    // セクション1: エンジン制御
    renderEngineControl();

    // セクション2: VisionDecisionEngine
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Vision Decision Engine")) {
        renderVisionStates();
    }

    // セクション3: テンプレートマッチ一覧
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Template Matches", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderMatchResults();
    }

    // セクション4: LearningMode
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Learning Mode")) {
        renderLearningMode();
    }

    // セクション5: オーバーレイ設定
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Match Overlay")) {
        renderOverlaySettings();
    }

    ImGui::End();
}

void init() {
    MLOG_INFO("ai_panel", "AI Panel 初期化");
    // LearningMode作成（まだ存在しない場合）
    if (!g_learning_mode) {
        g_learning_mode = std::make_unique<mirage::ai::LearningMode>();
        MLOG_INFO("ai_panel", "LearningMode 作成完了");
    }
}

void shutdown() {
    MLOG_INFO("ai_panel", "AI Panel シャットダウン");
    // LearningMode停止はMirageContext::shutdownで実施
}

} // namespace mirage::gui::ai_panel

#else // !USE_AI

namespace mirage::gui::ai_panel {
void renderAIPanel() {}
void init() {}
void shutdown() {}
} // namespace mirage::gui::ai_panel

#endif // USE_AI
