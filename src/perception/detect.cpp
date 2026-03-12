// =============================================================================
// MirageSystem - Perception Layer: detect() (impl)
// =============================================================================

#include "detect.hpp"
#include "template_matcher.hpp"

namespace mirage::perception {

// ローカルヘルパー: TemplateMatchResult -> UiElementHit 変換
static UiElementHit fromResult(const TemplateMatchResult& r) {
    UiElementHit hit;
    hit.type       = "template";
    hit.label      = r.template_id;
    hit.bounds     = r.bounds;
    hit.confidence = r.confidence;
    hit.frame_info = r.frame_info;
    return hit;
}

DetectionBundle detect(mirage::vk::VulkanTemplateMatcher& matcher,
                       const Frame& frame,
                       const ITemplateSource& store) {
    DetectionBundle bundle;
    bundle.frame = frameInfoOf(frame);

    const size_t n = store.size();
    for (size_t i = 0; i < n; ++i) {
        auto result = matchTemplate(matcher, frame, store.at(i));
        if (result) {
            bundle.template_results.push_back(*result);
            bundle.elements.push_back(fromResult(*result));
        }
    }

    // ocr_results は将来実装 — 常に空で返す

    return bundle;
}

} // namespace mirage::perception
