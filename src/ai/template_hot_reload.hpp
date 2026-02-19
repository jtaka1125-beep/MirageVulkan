#pragma once
// =============================================================================
// テンプレートホットリロード - 実行中テンプレート更新
// MirageComplete/src/ai/template_hot_reload から移行
// D3D11 + GpuTemplateMatcherMVP → VulkanTemplateMatcher
// =============================================================================
#include "ai/template_store.hpp"
#include "ai/template_manifest.hpp"
#include <string>

namespace mirage::vk {
class VulkanTemplateMatcher;
}

namespace mirage::ai {

struct HotReloadConfig {
    std::string templates_dir = "templates";
    std::string manifest_path = "templates/manifest.json";
    int id_start = 1;
};

struct HotReloadResult {
    bool ok = false;
    std::string error;
    int template_id = -1;
    std::string file_rel;
};

// テンプレートの追加/更新: Store + Matcher + Manifest を一括更新
HotReloadResult addOrUpdateTemplateAndRegister(
    TemplateStore& store,
    mirage::vk::VulkanTemplateMatcher& matcher,
    const HotReloadConfig& cfg,
    const std::string& name,
    const std::string& file_rel,
    int w, int h);

} // namespace mirage::ai
