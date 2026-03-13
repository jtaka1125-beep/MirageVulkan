#include "detect.hpp"
#include "template_matcher.hpp"
#include "vulkan_template_matcher.hpp"
#include "mirage_log.hpp"
#include <unordered_map>

namespace mirage::perception {

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

    if (!frame.data || frame.data_size == 0 || store.size() == 0) {
        return bundle;
    }

    // matcher.match() を1回だけ呼ぶ（全テンプレートを一括GPU処理）
    auto res = matcher.match(frame.data, frame.width, frame.height);
    if (!res) {
        MLOG_WARN("perception", "detect: match failed: %s", res.error().message.c_str());
        return bundle;
    }

    const auto& hits = res.value();

    // template_id ごとにベストヒットを集める
    std::unordered_map<int, const mirage::vk::VkMatchResult*> best_per_template;
    for (const auto& h : hits) {
        auto it = best_per_template.find(h.template_id);
        if (it == best_per_template.end() || h.score > it->second->score) {
            best_per_template[h.template_id] = &h;
        }
    }

    // 各テンプレートの best hit を TemplateMatchResult に変換
    for (const auto& [tid, best] : best_per_template) {
        std::string name = matcher.getTemplateName(tid);
        if (name.empty()) continue;

        TemplateMatchResult r;
        r.template_id  = name;
        r.score        = best->score;
        r.confidence   = best->score;
        r.bounds.x     = best->x;
        r.bounds.y     = best->y;
        r.bounds.w     = best->template_width;
        r.bounds.h     = best->template_height;
        r.frame_info   = frameInfoOf(frame);

        bundle.template_results.push_back(r);
        bundle.elements.push_back(fromResult(r));
    }

    MLOG_INFO("perception", "detect() done: templates=%zu results=%zu",
              store.size(), bundle.elements.size());

    return bundle;
}

} // namespace mirage::perception
