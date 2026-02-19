#pragma once
// =============================================================================
// テンプレートオートスキャン - templatesフォルダ監視・マニフェスト同期
// MirageComplete/src/ai/template_autoscan から移行 (WIC → stb_image)
// =============================================================================
#include "ai/template_manifest.hpp"
#include <string>

namespace mirage::ai {

struct AutoScanConfig {
    std::string templates_dir = "templates";
    std::string manifest_path = "templates/manifest.json";
    int id_start = 1;
    bool allow_png = true;
    bool allow_jpg = true;
    bool allow_bmp = true;
};

struct AutoScanResult {
    bool ok = false;
    std::string error;
    int added = 0;
    int updated = 0;
    int removed = 0;
    int kept = 0;
};

// templatesディレクトリをスキャンしてマニフェストを同期
AutoScanResult syncTemplateManifest(const AutoScanConfig& cfg, TemplateManifest& out_manifest);

} // namespace mirage::ai
