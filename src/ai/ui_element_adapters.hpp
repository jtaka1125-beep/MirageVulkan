#pragma once
// =============================================================================
// ui_element_adapters.hpp
// VkMatchResult / Layer2Result -> UiElementHit 変換アダプタ
// =============================================================================
#include "ai/ui_element.hpp"
#include "ai/layer2_input.hpp"
#include "vulkan_template_matcher.hpp"
#include "ai/vision_decision_engine.hpp"

namespace mirage::ai {

// ---------------------------------------------------------------------------
// VkMatchResult -> UiElementHit
// template_name: TemplateStore or manifest から引いた名前を渡す
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
    // confidence: NCCスコアをそのまま使う (0.0-1.0)
    return UiElementHit::make(
        "template",
        template_name,
        bbox,
        m.score,                        // confidence
        UiElementSource::TemplateNcc,
        m.score,                        // raw_score
        screen_w, screen_h
    );
}

// ---------------------------------------------------------------------------
// Layer2Result -> UiElementHit
// found=false の場合は confidence=0 で返す（呼び出し側でフィルタ推奨）
// ---------------------------------------------------------------------------
inline UiElementHit fromLayer2Result(
    const VisionDecisionEngine::Layer2Result& r,
    int screen_w, int screen_h)
{
    Rect bbox{
        r.x - 40,  // ボタン座標は中心なのでおよそのbboxに変換
        r.y - 20,
        80, 40
    };
    // xが負になるのを防ぐ
    if (bbox.x < 0) bbox.x = 0;
    if (bbox.y < 0) bbox.y = 0;

    float conf = r.found ? 0.85f : 0.0f;  // Layer2は高信頼（モデル判定）
    std::string label = r.type + ":" + r.button_text;

    return UiElementHit::make(
        "button",
        label,
        bbox,
        conf,
        UiElementSource::Ocr,   // LLM vision由来だがテキスト系として扱う
        conf,
        screen_w, screen_h
    );
}

// ---------------------------------------------------------------------------
// Layer2Input ビルダー
// vk_results:   VulkanTemplateMatcher の出力 (複数)
// name_lookup:  template_id -> name の変換関数
// l2:           VisionDecisionEngine::Layer2Result (任意)
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

    // VkMatchResult を変換
    for (const auto& m : vk_results) {
        std::string name = name_lookup(m.template_id);
        inp.elements.push_back(fromVkMatch(m, name, screen_w, screen_h));
    }

    // Layer2Result を追加（found の場合のみ）
    if (l2 && l2->found) {
        inp.elements.push_back(fromLayer2Result(*l2, screen_w, screen_h));
    }

    // dedup
    inp.elements = deduplicateElements(std::move(inp.elements));

    // 候補アクション生成（要素ごとに tap アクションを生成）
    for (int i = 0; i < (int)inp.elements.size(); ++i) {
        ActionCandidate cand;
        cand.action       = "tap:" + inp.elements[i].text;
        cand.source_index = i;
        cand.priority     = inp.elements[i].confidence;
        inp.candidates.push_back(cand);
    }
    // priority降順ソート
    std::sort(inp.candidates.begin(), inp.candidates.end(),
        [](const ActionCandidate& a, const ActionCandidate& b){
            return a.priority > b.priority;
        });

    return inp;
}

} // namespace mirage::ai
