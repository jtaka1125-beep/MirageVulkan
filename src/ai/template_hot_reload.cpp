// =============================================================================
// テンプレートホットリロード - Store + Matcher + Manifest 一括更新
// =============================================================================
#include "ai/template_hot_reload.hpp"
#include "ai/template_autoscan.hpp"
#include "vulkan_template_matcher.hpp"
#include "mirage_log.hpp"

#include <filesystem>

namespace fs = std::filesystem;

static constexpr const char* TAG = "TplHotReload";

namespace mirage::ai {

HotReloadResult addOrUpdateTemplateAndRegister(
    TemplateStore& store,
    mirage::vk::VulkanTemplateMatcher& matcher,
    const HotReloadConfig& cfg,
    const std::string& name,
    const std::string& file_rel,
    int w, int h)
{
    HotReloadResult r;
    if (name.empty() || file_rel.empty() || w <= 0 || h <= 0) {
        r.error = "bad args";
        return r;
    }

    // 1) 既存マニフェスト読込（なければ新規作成）
    TemplateManifest m;
    {
        std::string err;
        if (!loadManifestJson(cfg.manifest_path, m, &err)) {
            m.version = 1;
            m.root_dir = cfg.templates_dir;
        }
    }

    // 2) file_rel で検索（安定キー）
    int existingIndex = -1;
    for (size_t i = 0; i < m.entries.size(); ++i) {
        if (m.entries[i].file == file_rel) {
            existingIndex = (int)i;
            break;
        }
    }

    if (existingIndex < 0) {
        // 新規ID割当
        int newId = allocateNextId(m, cfg.id_start);

        TemplateEntry e;
        e.template_id = newId;
        e.name = name;
        e.file = file_rel;
        e.w = w;
        e.h = h;
        e.mtime_utc = 0;
        e.crc32 = 0;
        e.tags = "";
        m.entries.push_back(e);

        r.template_id = newId;
        MLOG_INFO(TAG, "新規テンプレート: id=%d name=%s", newId, name.c_str());
    } else {
        // 既存更新
        auto& e = m.entries[(size_t)existingIndex];
        e.name = name;
        e.w = w;
        e.h = h;
        r.template_id = e.template_id;
        MLOG_INFO(TAG, "テンプレート更新: id=%d name=%s", r.template_id, name.c_str());
    }

    // 3) マニフェスト保存
    {
        std::string err;
        if (!saveManifestJson(cfg.manifest_path, m, &err)) {
            r.error = "マニフェスト保存失敗: " + err;
            MLOG_ERROR(TAG, "%s", r.error.c_str());
            return r;
        }
    }

    // 4) TemplateStore にファイルから読込
    std::string full_path = cfg.templates_dir + "/" + file_rel;
    {
        auto loadResult = store.loadFromFile(r.template_id, full_path);
        if (loadResult.is_err()) {
            r.error = "store.loadFromFile失敗: " + loadResult.error().message;
            MLOG_ERROR(TAG, "%s", r.error.c_str());
            return r;
        }
    }

    // 5) VulkanTemplateMatcher に登録
    auto* th = store.get(r.template_id);
    if (!th || th->gray_data.empty()) {
        r.error = "store.get()失敗またはgray_dataが空";
        MLOG_ERROR(TAG, "%s", r.error.c_str());
        return r;
    }

    auto addResult = matcher.addTemplate(
        th->source_path_utf8,
        th->gray_data.data(),
        th->w, th->h,
        ""  // group（デフォルト）
    );
    if (addResult.is_err()) {
        r.error = "matcher.addTemplate失敗: " + addResult.error().message;
        MLOG_ERROR(TAG, "%s", r.error.c_str());
        return r;
    }
    int matcher_id = addResult.value();

    MLOG_INFO(TAG, "Matcher登録完了: template_id=%d matcher_id=%d %s",
              r.template_id, matcher_id, full_path.c_str());

    r.ok = true;
    r.file_rel = file_rel;
    return r;
}

} // namespace mirage::ai
