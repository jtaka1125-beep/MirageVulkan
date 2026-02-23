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
#include <cstring>
#include <algorithm>
#include "event_bus.hpp"

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
// LearningMode キャプチャ状態 (改善I)
// =============================================================================
struct CaptureState {
    int  slot       = 0;              // キャプチャ対象スロット
    char name[64]   = "template_001"; // テンプレート名
    int  roi_x      = 0;
    int  roi_y      = 0;
    int  roi_w      = 200;
    int  roi_h      = 200;
    bool capturing  = false;          // キャプチャ中フラグ
    // 最後の結果
    bool  last_ok   = false;
    bool  has_result = false;
    char  last_msg[256] = {};
    int   last_tid  = -1;
    int   last_w    = 0;
    int   last_h    = 0;
};
static CaptureState s_cap;
static mirage::SubscriptionHandle s_cap_sub;  // LearningCaptureEvent 購読ハンドル

static void ensureCapSub() {
    if (static_cast<bool>(s_cap_sub)) return;  // GCC15: explicit cast for SubscriptionHandle
    s_cap_sub = mirage::bus().subscribe<mirage::LearningCaptureEvent>(
        [](const mirage::LearningCaptureEvent& e) {
            s_cap.capturing = false;
            s_cap.has_result = true;
            s_cap.last_ok    = e.ok;
            s_cap.last_tid   = e.template_id;
            s_cap.last_w     = e.w;
            s_cap.last_h     = e.h;
            if (e.ok) {
                snprintf(s_cap.last_msg, sizeof(s_cap.last_msg),
                    "OK: id=%d  %dx%d  %s",
                    e.template_id, e.w, e.h, e.saved_file_rel.c_str());
                // 名前をインクリメント（template_001 -> template_002）
                std::string name(s_cap.name);
                auto pos = name.rfind('_');
                if (pos != std::string::npos) {
                    try {
                        int n = std::stoi(name.substr(pos + 1));
                        char next[64];
                        snprintf(next, sizeof(next), "%s_%03d",
                                 name.substr(0, pos).c_str(), n + 1);
                        strncpy(s_cap.name, next, sizeof(s_cap.name) - 1);
                    } catch (...) {}
                }
            } else {
                snprintf(s_cap.last_msg, sizeof(s_cap.last_msg),
                    "NG: %s", e.error.c_str());
            }
        });
}

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

    // 改善K: テンプレート別ヒット率
    if (!stats.template_stats.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Template Hit Rate (改善K)");
        if (ImGui::BeginTable("tpl_stats", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 120))) {
            ImGui::TableSetupColumn("Template",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Detect",      ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Actions",     ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Act Rate",    ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableHeadersRow();
            for (const auto& [name, ts] : stats.template_stats) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(name.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%llu", (unsigned long long)ts.detect_count);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", (unsigned long long)ts.action_count);
                ImGui::TableSetColumnIndex(3);
                float ar = ts.action_rate();
                ImVec4 col = ar > 0.7f ? ImVec4(0.2f,1.0f,0.2f,1.0f)
                           : ar > 0.3f ? ImVec4(1.0f,0.8f,0.2f,1.0f)
                                       : ImVec4(1.0f,0.4f,0.4f,1.0f);
                ImGui::TextColored(col, "%.0f%%", ar * 100.0f);
            }
            ImGui::EndTable();
        }
    }
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
    // VDE設定スライダー
    // ----------------------------------------------------------------
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("VDE設定")) {
        auto vde_cfg = g_ai_engine->getVDEConfig();
        bool changed = false;

        ImGui::PushItemWidth(180);

        changed |= ImGui::SliderInt("Confirm Count##vde", &vde_cfg.confirm_count, 1, 20);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("DETECTED -> CONFIRMED に必要な連続検出回数\n(少ない=速い反応, 多い=誤検出防止)");

        changed |= ImGui::SliderInt("Cooldown (ms)##vde", &vde_cfg.cooldown_ms, 0, 5000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("アクション実行後の冷却時間 (同一テンプレート再実行防止)");

        changed |= ImGui::SliderInt("Debounce (ms)##vde", &vde_cfg.debounce_window_ms, 0, 2000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("デバウンスウィンドウ (同一アクションの連打抑制)");

        changed |= ImGui::SliderInt("Error Recovery (ms)##vde", &vde_cfg.error_recovery_ms, 0, 10000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ERROR_RECOVERY 状態の最大滞在時間");

        ImGui::PopItemWidth();

        if (changed) {
            g_ai_engine->setVDEConfig(vde_cfg);
        }

        // 改善L: Jitter
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f,0.8f,0.4f,1.0f),"Jitter Delay (改善L)");
        {
            static int s_jmin=0,s_jmax=0; bool jit=false;
            ImGui::SetNextItemWidth(130); jit|=ImGui::SliderInt("Min(ms)##jit",&s_jmin,0,1000);
            ImGui::SetNextItemWidth(130); jit|=ImGui::SliderInt("Max(ms)##jit2",&s_jmax,0,2000);
            s_jmax=std::max(s_jmax,s_jmin);
            if(jit) g_ai_engine->setJitterConfig(s_jmin,s_jmax);
            if(s_jmax>0) ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.0f),"jitter:%d~%dms",s_jmin,s_jmax);
        }
        // 改善M: Hot Reload
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f,0.8f,0.4f,1.0f),"Hot Reload (改善M)");
        {
            static bool s_hr=false; static int s_hri=1000; bool ch2=false;
            ch2|=ImGui::Checkbox("manifest.json 監視##hr",&s_hr);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("変更検知してテンプレート自動再登録");
            ImGui::SameLine(); ImGui::SetNextItemWidth(100);
            ch2|=ImGui::SliderInt("間隔(ms)##hri",&s_hri,200,5000);
            if(ch2) g_ai_engine->setHotReload(s_hr,s_hri);
            if(s_hr){ImGui::SameLine();ImGui::TextColored(ImVec4(0.2f,1.0f,0.2f,1.0f),"[監視中]");}
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

        // スコア履歴グラフ
        if (hist.count > 1) {
            char plot_id[64];
            snprintf(plot_id, sizeof(plot_id), "##score_%s", m.template_id.c_str());
            ImGui::SameLine();
            ImGui::PlotLines(plot_id, hist.buf, 60, hist.head,
                nullptr, 0.f, 1.f, ImVec2(80, 20));
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

    // EventBus 購読を確保
    ensureCapSub();

    bool running = g_learning_mode->is_running();

    // ── Start/Stop トグル ──────────────────────────────────────────
    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("Stop Learning", ImVec2(120, 25))) g_learning_mode->stop();
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.1f, 0.5f, 0.1f, 1.0f));
        if (ImGui::Button("Start Learning", ImVec2(120, 25))) g_learning_mode->start();
        ImGui::PopStyleColor(3);
    }
    ImGui::SameLine();
    ImGui::TextColored(
        running ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
        running ? "Running" : "Stopped");

    if (!running) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.3f, 1.0f),
            "(*) Start Learning してからキャプチャしてください");
        return;
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Template Capture (改善I)");

    // ── スロット選択 ───────────────────────────────────────────────
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("Slot##cap_slot", &s_cap.slot);
    s_cap.slot = std::max(0, std::min(s_cap.slot, 9));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("キャプチャ対象スロット番号 (0-9)");

    // ── テンプレート名 ─────────────────────────────────────────────
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Name##cap_name", s_cap.name, sizeof(s_cap.name));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("保存するテンプレート名 (英数_のみ推奨)\nキャプチャ成功後に末尾番号が自動インクリメント");

    // ── ROI 入力（2列レイアウト） ──────────────────────────────────
    ImGui::Text("ROI (px):");
    ImGui::SetNextItemWidth(80); ImGui::InputInt("X##cap_x", &s_cap.roi_x);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80); ImGui::InputInt("Y##cap_y", &s_cap.roi_y);
    ImGui::SetNextItemWidth(80); ImGui::InputInt("W##cap_w", &s_cap.roi_w);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80); ImGui::InputInt("H##cap_h", &s_cap.roi_h);

    // 値クランプ
    s_cap.roi_x = std::max(0, s_cap.roi_x);
    s_cap.roi_y = std::max(0, s_cap.roi_y);
    s_cap.roi_w = std::max(4, s_cap.roi_w);
    s_cap.roi_h = std::max(4, s_cap.roi_h);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("最終マッチ結果の座標を参考に範囲を指定してください");

    // ── 最新マッチ座標をROIにコピー ───────────────────────────────
    if (g_ai_engine) {
        auto matches = g_ai_engine->getLastMatches();
        if (!matches.empty()) {
            const auto& m = matches[0];
            ImGui::SameLine();
            if (ImGui::SmallButton("From Match##cap_from")) {
                // マッチ中心 ± テンプレートサイズをROIに設定
                s_cap.roi_x = m.x;
                s_cap.roi_y = m.y;
                s_cap.roi_w = std::max(4, m.w);
                s_cap.roi_h = std::max(4, m.h);
                strncpy(s_cap.name, m.template_id.c_str(), sizeof(s_cap.name) - 1);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("最新マッチ結果 (%s) の座標・サイズをROIにコピー", m.template_id.c_str());
        }
    }

    // ── Capture ボタン ─────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::BeginDisabled(s_cap.capturing);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.4f, 0.9f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.1f, 0.3f, 0.8f, 1.0f));
    if (ImGui::Button("Capture Template", ImVec2(150, 28))) {
        mirage::LearningStartEvent evt;
        evt.device_id  = "slot_" + std::to_string(s_cap.slot);
        evt.name_stem  = std::string(s_cap.name);
        evt.roi_x      = s_cap.roi_x;
        evt.roi_y      = s_cap.roi_y;
        evt.roi_w      = s_cap.roi_w;
        evt.roi_h      = s_cap.roi_h;
        s_cap.capturing  = true;
        s_cap.has_result = false;
        mirage::bus().publish(evt);
        MLOG_INFO("ai_panel", "LearningStartEvent発行: device=%s name=%s roi=(%d,%d %dx%d)",
                  evt.device_id.c_str(), evt.name_stem.c_str(),
                  evt.roi_x, evt.roi_y, evt.roi_w, evt.roi_h);
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    if (s_cap.capturing) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "処理中...");
    }

    // ── 結果表示 ───────────────────────────────────────────────────
    if (s_cap.has_result) {
        ImVec4 result_col = s_cap.last_ok
            ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f)
            : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        ImGui::TextColored(result_col, "%s", s_cap.last_msg);
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
