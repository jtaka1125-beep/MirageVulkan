// =============================================================================
// テンプレートオートスキャン - ディレクトリ走査 + マニフェスト更新
// =============================================================================
#include "ai/template_autoscan.hpp"
#include "mirage_log.hpp"
#include "stb_image.h"

#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

static constexpr const char* TAG = "TplAutoScan";

namespace mirage::ai {

static bool isAllowed(const fs::path& p, const AutoScanConfig& cfg) {
    auto ext = p.extension().string();
    // 小文字化
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".png") return cfg.allow_png;
    if (ext == ".jpg" || ext == ".jpeg") return cfg.allow_jpg;
    if (ext == ".bmp") return cfg.allow_bmp;
    return false;
}

static uint64_t toU64FileTimeUTC(const fs::file_time_type& ft) {
    // 変更検出用の単純変換（同一マシン内比較）
    return (uint64_t)ft.time_since_epoch().count();
}

// stbi_infoで画像サイズだけ取得（デコード不要）
static bool getImageDimensions(const std::string& path, int& w, int& h) {
    int channels = 0;
    if (stbi_info(path.c_str(), &w, &h, &channels)) {
        return true;
    }
    return false;
}

AutoScanResult syncTemplateManifest(const AutoScanConfig& cfg, TemplateManifest& out_manifest) {
    AutoScanResult r;
    out_manifest = {};
    TemplateManifest m;

    // 既存マニフェスト読込
    {
        std::string err;
        TemplateManifest tmp;
        if (loadManifestJson(cfg.manifest_path, tmp, &err)) {
            m = std::move(tmp);
        } else {
            m.version = 1;
            m.root_dir = cfg.templates_dir;
        }
    }

    // file -> entry index マップ構築
    std::unordered_map<std::string, size_t> byFile;
    for (size_t i = 0; i < m.entries.size(); ++i) {
        byFile[m.entries[i].file] = i;
    }

    // ディレクトリスキャン
    std::unordered_set<std::string> seen;
    if (!fs::exists(cfg.templates_dir)) {
        r.ok = false;
        r.error = "templatesディレクトリ未発見: " + cfg.templates_dir;
        MLOG_ERROR(TAG, "%s", r.error.c_str());
        return r;
    }

    for (auto& it : fs::recursive_directory_iterator(cfg.templates_dir)) {
        if (!it.is_regular_file()) continue;
        auto p = it.path();
        if (!isAllowed(p, cfg)) continue;

        // templatesディレクトリからの相対パス
        auto rel = fs::relative(p, cfg.templates_dir).generic_string();
        seen.insert(rel);

        auto mtime = toU64FileTimeUTC(fs::last_write_time(p));

        auto found = byFile.find(rel);
        if (found == byFile.end()) {
            // 新規エントリ
            int newId = allocateNextId(m, cfg.id_start);

            // stbi_infoで画像サイズ取得
            int w = 0, h = 0;
            if (!getImageDimensions(p.string(), w, h)) {
                MLOG_WARN(TAG, "画像サイズ取得失敗、スキップ: %s", rel.c_str());
                continue;
            }

            TemplateEntry e;
            e.template_id = newId;
            e.name = p.stem().string();
            e.file = rel;
            e.w = w;
            e.h = h;
            e.mtime_utc = mtime;
            e.crc32 = 0;
            e.tags = "";
            m.entries.push_back(std::move(e));

            r.added++;
        } else {
            // 既存: mtime確認
            auto& e = m.entries[found->second];
            if (e.mtime_utc != mtime) {
                // サイズ再取得
                int w = 0, h = 0;
                if (getImageDimensions(p.string(), w, h)) {
                    e.w = w;
                    e.h = h;
                }
                e.mtime_utc = mtime;
                r.updated++;
            } else {
                r.kept++;
            }
        }
    }

    // 削除されたファイルのエントリを除去
    {
        std::vector<TemplateEntry> kept;
        kept.reserve(m.entries.size());
        for (auto& e : m.entries) {
            if (seen.count(e.file)) {
                kept.push_back(e);
            } else {
                r.removed++;
            }
        }
        m.entries.swap(kept);
    }

    // マニフェスト保存
    {
        std::string err;
        if (!saveManifestJson(cfg.manifest_path, m, &err)) {
            r.ok = false;
            r.error = "マニフェスト保存失敗: " + err;
            MLOG_ERROR(TAG, "%s", r.error.c_str());
            return r;
        }
    }

    out_manifest = std::move(m);
    r.ok = true;

    MLOG_INFO(TAG, "スキャン完了: 追加=%d 更新=%d 保持=%d 削除=%d",
              r.added, r.updated, r.kept, r.removed);
    return r;
}

} // namespace mirage::ai
