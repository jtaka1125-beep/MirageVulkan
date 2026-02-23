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
#include <ctime>



namespace mirage::ai {



struct TemplateHandle {

    int template_id = -1;

    int w = 0;

    int h = 0;

    std::vector<uint8_t> gray_data;     // Gray8 ピクセルデータ（GPU登録用に保持）

    std::string source_path_utf8;

    std::string debug;

    int matcher_id = -1;                // VulkanTemplateMatcher 内のID

    // 改善E: 検索ROI (正規化座標、roi_w=0 で全画面)

    float roi_x = 0.0f, roi_y = 0.0f, roi_w = 0.0f, roi_h = 0.0f;
    // 改善P: バージョン管理
    int      version    = 1;
    uint32_t checksum   = 0;
    std::string added_at;
    std::string updated_at;

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



    // 改善P: 変更ログ
    struct ChangeLogEntry {
        int template_id;
        int version;
        uint32_t checksum;
        std::string timestamp;
        std::string event;
    };
    const std::vector<ChangeLogEntry>& getChangeLogs() const;
    int getTemplateVersion(int template_id) const;

private:

    std::unordered_map<int, TemplateHandle> map_;

    std::vector<ChangeLogEntry> change_log_;
    TemplateStoreConfig cfg_;

};



} // namespace mirage::ai

