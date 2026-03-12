// =============================================================================
// MirageSystem - Perception Layer: Template Matcher (impl)
// =============================================================================

#include "template_matcher.hpp"
#include "vulkan_template_matcher.hpp"  // mirage::vk::VulkanTemplateMatcher
#include "mirage_log.hpp"

namespace mirage::perception {

std::optional<TemplateMatchResult>
matchTemplate(mirage::vk::VulkanTemplateMatcher& matcher,
              const Frame& frame,
              const Template& templ) {
    if (!frame.data || frame.data_size == 0) {
        MLOG_WARN("perception", "matchTemplate: frame data is empty");
        return std::nullopt;
    }

    // frame.data は gray8 (width*height bytes) であることを期待する。
    // 呼び出し元が変換済みデータを渡す責務を持つ。
    auto res = matcher.match(frame.data, frame.width, frame.height);
    if (!res) {
        MLOG_WARN("perception", "matchTemplate: VulkanTemplateMatcher::match failed: %s",
               res.error().message.c_str());
        return std::nullopt;
    }

    const auto& hits = res.value();
    if (hits.empty()) {
        return std::nullopt;
    }

    // スコア最大のヒットを採用
    const mirage::vk::VkMatchResult* best = &hits[0];
    for (const auto& h : hits) {
        if (h.score > best->score) best = &h;
    }

    TemplateMatchResult r;
    r.template_id  = templ.name;
    r.score        = best->score;
    r.confidence   = best->score;  // raw NCC スコアをそのまま confidence として使用
    r.bounds       = { best->x, best->y, best->template_width, best->template_height };
    r.frame_info   = frameInfoOf(frame);
    return r;
}

std::optional<UiElementHit>
matchTemplateAsHit(mirage::vk::VulkanTemplateMatcher& matcher,
                   const Frame& frame,
                   const Template& templ) {
    auto r = matchTemplate(matcher, frame, templ);
    if (!r) return std::nullopt;

    UiElementHit hit;
    hit.type       = "template";
    hit.label      = r->template_id;
    hit.bounds     = r->bounds;
    hit.confidence = r->confidence;
    hit.frame_info = r->frame_info;
    return hit;
}

} // namespace mirage::perception
