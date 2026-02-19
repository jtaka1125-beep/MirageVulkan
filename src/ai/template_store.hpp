#pragma once
// =============================================================================
// テンプレートストア - Vulkan GPU テンプレート管理
// MirageComplete/src/ai/template_store から移行 (D3D11 SRV → VulkanImage)
// =============================================================================
#include "result.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace mirage::ai {

struct TemplateHandle {
    int template_id = -1;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> gray_data;     // Gray8 ピクセルデータ（GPU登録用に保持）
    std::string source_path_utf8;
    std::string debug;
    int matcher_id = -1;                // VulkanTemplateMatcher 内のID
};

struct TemplateStoreConfig {
    bool prefer_gray8 = true;
};

class TemplateStore {
public:
    TemplateStore() = default;
    void setConfig(const TemplateStoreConfig& cfg) { cfg_ = cfg; }

    // stbi_loadでファイルからGray8読み込み、内部保持
    mirage::Result<void> loadFromFile(int template_id, const std::string& path_utf8);

    // 直接Gray8データを登録
    mirage::Result<void> registerGray8(int template_id, const uint8_t* gray_data, int w, int h,
                                       const std::string& src_path_utf8);

    const TemplateHandle* get(int template_id) const;
    std::vector<int> listTemplateIds() const;
    void clear();
    void remove(int template_id);
    size_t size() const { return map_.size(); }

private:
    std::unordered_map<int, TemplateHandle> map_;
    TemplateStoreConfig cfg_;
};

} // namespace mirage::ai
