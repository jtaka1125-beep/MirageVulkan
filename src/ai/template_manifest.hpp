#pragma once
// =============================================================================
// テンプレートマニフェスト - テンプレートID/メタデータ管理
// MirageComplete/src/ai/template_manifest から移行
// =============================================================================
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace mirage::ai {

struct TemplateEntry {
    int template_id = 0;
    std::string name;
    std::string file;       // templatesディレクトリからの相対パス
    int w = 0;
    int h = 0;
    uint64_t mtime_utc = 0;
    uint32_t crc32 = 0;
    std::string tags;
    // 改善E: 検索ROI (正規化座標 0.0-1.0、roi_w=0 で全画面)
    float roi_x = 0.0f;
    float roi_y = 0.0f;
    float roi_w = 0.0f;   // 0 = 全幅
    float roi_h = 0.0f;   // 0 = 全高
};

struct TemplateManifest {
    int version = 1;
    std::string root_dir;
    std::vector<TemplateEntry> entries;
};

bool loadManifestJson(const std::string& path_utf8, TemplateManifest& out, std::string* err = nullptr);
bool saveManifestJson(const std::string& path_utf8, const TemplateManifest& m, std::string* err = nullptr);
std::unordered_map<int, size_t> indexById(const TemplateManifest& m);
int allocateNextId(const TemplateManifest& m, int start_id = 1);

} // namespace mirage::ai
