#pragma once
// =============================================================================
// ui_element_adapters.hpp
// VkMatchResult / Layer2Result -> UiElementHit 変換アダプタ
// Step3: dedup本流接続・保守的閾値・dedup前後ログ追加
// =============================================================================
#include "ai/ui_element.hpp"
#include "ai/layer2_input.hpp"
#include "vulkan_template_matcher.hpp"
#include "ai/vision_decision_engine.hpp"
#include "mirage_log.hpp"

namespace mirage::ai {

// ---------------------------------------------------------------------------
// dedup閾値（保守的設定: 誤統合回避優先）
// 実績を見て将来緩める
// ---------------------------------------------------------------------------
constexpr float DEDUP_IOU_THRESHOLD  = 0.5f;   // IoU: 50%以上で同一とみなす
constexpr float DEDUP_CENTER_PX_THR  = 24.f;   // 中心距離: 24px以内
constexpr float DEDUP_TEXT_SIM_THR   = 0.85f;  // テキスト類似度: 85%以上

// ---------------------------------------------------------------------------
// VkMatchResult -> UiElementHit
// ---------------------------------------------------------------------------
inline UiElementHit fromVkMatch(const mirage::vk::VkMatchResult& m,
                                 const std::string& template_name,
                                 int screen_w, int screen_h)
{
    Rect bbox{
        m.x,
        m.y,
        m.template_width  > 0 ? m.template_width  : 1,
        m.template_height > 0 ? m.template_height : 1
    };
    return UiElementHit::make(
        "template",
        template_name,
        bbox,
        m.score,
        UiElementSource::TemplateNcc,
        m.score,
        screen_w, screen_h
    );
}

// ---------------------------------------------------------------------------
// Layer2Result -> UiElementHit
// ---------------------------------------------------------------------------
inline UiElementHit fromLayer2Result(
    const VisionDecisionEngine::Layer2Result& r,
    int screen_w, int screen_h)
{
    Rect bbox{ r.x - 40, r.y - 20, 80, 40 };
    if (bbox.x < 0) bbox.x = 0;
    if (bbox.y < 0) bbox.y = 0;

    float conf = r.found ? 0.85f : 0.0f;
    std::string label = r.type + ":" + r.button_text;

    return UiElementHit::make(
        "button",
        label,
        bbox,
        conf,
        UiElementSource::Ocr,
        conf,
        screen_w, screen_h
    );
}

// ---------------------------------------------------------------------------
// Layer2Input ビルダー
// Step3: raw収集 -> dedup -> candidate生成 の順を明示
// ---------------------------------------------------------------------------
inline Layer2Input buildLayer2Input(
    const std::vector<mirage::vk::VkMatchResult>& vk_results,
    const std::function<std::string(int)>& name_lookup,
    const VisionDecisionEngine::Layer2Result* l2,
    int screen_w, int screen_h,
    const std::string& previous_state = {},
    const std::string& task_goal = {})
{
    Layer2Input inp;
    inp.previous_state = previous_state;
    inp.task_goal      = task_goal;

    // --- Step1: raw elements 収集 ---
    std::vector<UiElementHit> raw;
    raw.reserve(vk_results.size() + 1);

    for (const auto& m : vk_results) {
        raw.push_back(fromVkMatch(m, name_lookup(m.template_id), screen_w, screen_h));
    }
    if (l2 && l2->found) {
        raw.push_back(fromLayer2Result(*l2, screen_w, screen_h));
    }

    // --- Step2: dedup（保守的閾値）---
    inp.elements = deduplicateElements(
        std::move(raw),
        DEDUP_IOU_THRESHOLD,
        DEDUP_CENTER_PX_THR,
        DEDUP_TEXT_SIM_THR
    );

    // dedup前後ログ（導入確認用・DEBUG level）
    {
        size_t raw_n = vk_results.size() + (l2 && l2->found ? 1 : 0);
        if (raw_n != inp.elements.size()) {
            MLOG_DEBUG("ai", "[Layer2Input] dedup: raw=%zu -> %zu (-%zu merged)",
                       raw_n, inp.elements.size(), raw_n - inp.elements.size());
        }
    }

    // --- Step3: candidate生成（dedup後）---
    inp.candidates.reserve(inp.elements.size());
    for (int i = 0; i < (int)inp.elements.size(); ++i) {
        ActionCandidate cand;
        cand.action       = "tap:" + inp.elements[i].text;
        cand.source_index = i;
        cand.priority     = inp.elements[i].confidence;
        inp.candidates.push_back(cand);
    }
    std::sort(inp.candidates.begin(), inp.candidates.end(),
        [](const ActionCandidate& a, const ActionCandidate& b){
            return a.priority > b.priority;
        });

    return inp;
}

} // namespace mirage::ai
