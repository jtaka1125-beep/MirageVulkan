#pragma once
// =============================================================================
// MirageSystem - Perception Layer: Template Matcher
// =============================================================================
// Thin wrapper around VulkanTemplateMatcher that converts results into
// the shared perception types (TemplateMatchResult / UiElementHit).
//
// Usage:
//   perception::Frame frame{ ... };
//   perception::Template tmpl{ "ok_button", image_span, w, h };
//   auto result = perception::matchTemplate(vtm, frame, tmpl);
//   if (result) { // found }
// =============================================================================

#include "types.hpp"
#include <optional>
#include <string>

// Forward declare to avoid pulling in heavy Vulkan headers
namespace mirage::vk {
class VulkanTemplateMatcher;
} // namespace mirage::vk

namespace mirage::perception {

// Template descriptor (non-owning view of template image bytes)
struct Template {
    std::string    name;
    const uint8_t* image      = nullptr;  // non-owning; gray8 bytes (width*height)
    size_t         image_size = 0;
    int            width      = 0;
    int            height     = 0;
};

// Single template match — returns TemplateMatchResult with frame metadata.
// frame.data must be gray8 (width*height bytes) for VulkanTemplateMatcher.
// matcher must already be initialized.
std::optional<TemplateMatchResult>
matchTemplate(mirage::vk::VulkanTemplateMatcher& matcher,
              const Frame& frame,
              const Template& templ);

// Convenience: returns UiElementHit directly.
std::optional<UiElementHit>
matchTemplateAsHit(mirage::vk::VulkanTemplateMatcher& matcher,
                   const Frame& frame,
                   const Template& templ);

} // namespace mirage::perception
