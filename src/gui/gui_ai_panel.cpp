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
#include <unordered_map>
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
// スコア履歴リングバッファ (改善C)
// =============================================================================
static constexpr int SCORE_HISTORY_LEN = 90;  // 約3秒@30fps

struct ScoreHistory {
    float buf[SCORE_HISTORY_LEN] = {};
    int   head  = 0;
    int   count = 0;
    void push(float v) {
        buf[head] = v;
        head = (head + 1) % SCORE_HISTORY_LEN;
        if (count < SCORE_HISTORY_LEN) ++count;
    }
    float max_val() const {
        float m = 0.0f;
        for (int i = 0; i < count; ++i) m = m > buf[i] ? m : buf[i];
        return m;
    }
};
static std::unordered_map<std::string, ScoreHistory> s_score_histories;

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

    // ----------------------------------------------------------------
    // VDE設定スライダー (改善B)
    // ----------------------------------------------------------------
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("VDE Config##vde_cfg")) {
        auto vde_cfg = g_ai_engine->getVDEConfig();
        bool changed = false;

        ImGui::PushItemWidth(180);

        changed |= ImGui::SliderInt("Confirm Count##vde", &vde_cfg.confirm_count, 1, 10);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("DETECTED -> CONFIRMED に必要な連続検出回数\n(少ない=速い反応, 多い=誤検出防止)");

        changed |= ImGui::SliderInt("Cooldown (ms)##vde", &vde_cfg.cooldown_ms, 500, 10000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("アクション実行後の冷却時間 (同一テンプレート再実行防止)");

        changed |= ImGui::SliderInt("Debounce (ms)##vde", &vde_cfg.debounce_window_ms, 100, 2000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("デバウンスウィンドウ (同一アクションの連打抑制)");

        changed |= ImGui::SliderInt("Error Recovery (ms)##vde", &vde_cfg.error_recovery_ms, 500, 10000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ERROR_RECOVERY 状態の最大滞在時間");

        ImGui::PopItemWidth();

        // ── 改善D: EWMA ──
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Temporal Filter (EWMA)");
        changed |= ImGui::Checkbox("Enable EWMA##vde", &vde_cfg.enable_ewma);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("フレーム間スコアをEWMA平滑化して瞬間ノイズを除去");

        ImGui::BeginDisabled(!vde_cfg.enable_ewma);
        ImGui::SetNextItemWidth(180);
        changed |= ImGui::SliderFloat("EWMA Alpha##vde", &vde_cfg.ewma_alpha, 0.05f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("平滑化係数: 0.05=超スムーズ, 1.0=生スコアそのまま");

        ImGui::SetNextItemWidth(180);
        changed |= ImGui::SliderFloat("EWMA Confirm Thr##vde", &vde_cfg.ewma_confirm_thr,
                                      0.3f, 0.95f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("CONFIRMED遷移に必要なEWMAスコアの最低値");
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Temporal Filter (改善D)");

        changed |= ImGui::Checkbox("Enable EWMA##vde", &vde_cfg.enable_ewma);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("指数移動平均でスコアを平滑化。\nノイズフレームや画面遷移中の誤検出を抑制。");

        if (vde_cfg.enable_ewma) {
            ImGui::SetNextItemWidth(180);
            changed |= ImGui::SliderFloat("EWMA Alpha##vde", &vde_cfg.ewma_alpha, 0.1f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("平滑化係数 (0=強平滑/遅反応, 1=生スコア/即反応)\n推奨: 0.3~0.5");

            ImGui::SetNextItemWidth(180);
            changed |= ImGui::SliderFloat("EWMA Confirm Thr##vde", &vde_cfg.ewma_confirm_thr, 0.1f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("EWMAがこの値以上でないとCONFIRMED不可\n(閾値との二重ゲート)");
        }

        if (changed) {
            g_ai_engine->setVDEConfig(vde_cfg);
        }

        // 現在値を小さく表示
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "confirm=%d  cool=%dms  db=%dms  err=%dms",
            vde_cfg.confirm_count, vde_cfg.cooldown_ms,
            vde_cfg.debounce_window_ms, vde_cfg.error_recovery_ms);
        if (vde_cfg.enable_ewma) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                "ewma: alpha=%.2f  confirm_thr=%.2f",
                vde_cfg.ewma_alpha, vde_cfg.ewma_confirm_thr);
        }
        if (vde_cfg.enable_ewma) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                "ewma: alpha=%.2f  confirm_thr=%.2f",
                vde_cfg.ewma_alpha, vde_cfg.ewma_confirm_thr);
        }
    }
}

// =============================================================================
// セクション3: テンプレートマッチ一覧 (改善C: スコア履歴グラフ追加)
// =============================================================================

static void renderMatchResults() {
    if (!g_ai_engine) return;

    auto matches = g_ai_engine->getLastMatches();

    if (matches.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "マッチなし");
        // スコア履歴はゼロ更新（非表示にしない、ゆっくりフェードアウト的挙動）
        return;
    }

    ImGui::Text("%d matches:", (int)matches.size());

    for (const auto& m : matches) {
        // スコア履歴更新 (改善C)
        auto& hist = s_score_histories[m.template_id];
        hist.push(m.score);

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

        // スコア履歴グラフ (改善C)
        if (hist.count > 1) {
            char plot_id[64];
            snprintf(plot_id, sizeof(plot_id), "##score_%s", m.template_id.c_str());

            // 閾値ラインを示すオーバーレイテキスト
            char plot_overlay[16];
            snprintf(plot_overlay, sizeof(plot_overlay), "%.0f%%", m.score * 100.0f);

            ImGui::PushStyleColor(ImGuiCol_FrameBg,      ImVec4(0.1f, 0.1f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_PlotLines,
                above_threshold ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                                : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            ImGui::SameLine(160);
            ImGui::PlotLines(plot_id,
                hist.buf, SCORE_HISTORY_LEN, hist.head,
                plot_overlay,
                0.0f, 1.0f,
                ImVec2(120, 28));
            ImGui::PopStyleColor(2);

            // 閾値ラインの注釈（グラフ右に小さく）
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f),
                "thr=%.0f%%", s_overlay_threshold * 100.0f);
        }
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
    if (ImGui::SliderFloat("Threshold##overlay", &s_overlay_threshold, 0.5f, 1.0f, "%.2f")) {
        // 閾値変更時にスコア履歴をクリア（古い判定基準が混在しないように）
        s_score_histories.clear();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("マッチング判定閾値。変更するとスコア履歴がリセットされます。");
}

// =============================================================================
// メインパネル描画
// =============================================================================

void renderAIPanel() {
    if (!g_ai_engine) return;

    static bool first_frame = true;
    ImGuiIO& io = ImGui::GetIO();
    float panel_width = 380.0f;  // グラフ追加分で少し広く

    if (first_frame) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panel_width - 10, 200));
        ImGui::SetNextWindowCollapsed(true);
        first_frame = false;
    }

    if (!ImGui::Begin("AI Engine", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    // セクション1: エンジン制御
    renderEngineControl();

    // セクション2: VisionDecisionEngine (スライダー付き)
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Vision Decision Engine")) {
        renderVisionStates();
    }

    // セクション3: テンプレートマッチ一覧 (スコアグラフ付き)
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
    if (!g_learning_mode) {
        g_learning_mode = std::make_unique<mirage::ai::LearningMode>();
        MLOG_INFO("ai_panel", "LearningMode 作成完了");
    }
}

void shutdown() {
    MLOG_INFO("ai_panel", "AI Panel シャットダウン");
    s_score_histories.clear();
}

} // namespace mirage::gui::ai_panel

#else // !USE_AI

namespace mirage::gui::ai_panel {
void renderAIPanel() {}
void init() {}
void shutdown() {}
} // namespace mirage::gui::ai_panel

#endif // USE_AI
