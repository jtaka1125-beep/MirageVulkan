#pragma once
// =============================================================================
// MirageSystem - Perception Layer: detect()
// =============================================================================
// Stateless entry point for a full perception pass on one frame.
//
// ITemplateSource is passed by caller - perception layer holds no state.
//   MacroEngine passes its own template set.
//   AIEngine passes its own template set (adapter around existing TemplateStore).
//
// Initial implementation: template_results only.
//   ocr_results: always empty (future).
//   elements: template-derived only, no dedup.
// =============================================================================

#include "types.hpp"
#include "template_matcher.hpp"  // Template struct
#include <cstddef>

// Forward declare to avoid pulling in heavy Vulkan headers
namespace mirage::vk {
class VulkanTemplateMatcher;
} // namespace mirage::vk

namespace mirage::perception {

// ---------------------------------------------------------------------------
// ITemplateSource - non-owning interface for template iteration
// ---------------------------------------------------------------------------
// size() / at(i) avoids std::vector copy per detect() call.
// ---------------------------------------------------------------------------
class ITemplateSource {
public:
    virtual ~ITemplateSource() = default;
    virtual size_t size() const = 0;
    virtual const Template& at(size_t i) const = 0;
};

// ---------------------------------------------------------------------------
// detect()
// ---------------------------------------------------------------------------
// Runs a full perception pass on `frame` using templates from `store`.
// Returns raw DetectionBundle (no dedup, no time-axis logic).
// Caller is responsible for dedup / freshness checks / history management.
// ---------------------------------------------------------------------------
DetectionBundle detect(mirage::vk::VulkanTemplateMatcher& matcher,
                       const Frame& frame,
                       const ITemplateSource& store);

} // namespace mirage::perception
