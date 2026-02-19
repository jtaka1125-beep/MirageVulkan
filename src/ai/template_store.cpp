// =============================================================================
// テンプレートストア - stbi_loadによるファイル読込 + Gray8管理
// =============================================================================
#include "ai/template_store.hpp"
#include "mirage_log.hpp"
#include "stb_image.h"

#include <algorithm>
#include <cstring>

static constexpr const char* TAG = "TplStore";

namespace mirage::ai {

// RGBA → Gray8 変換 (luma近似: 0.299R + 0.587G + 0.114B)
static inline uint8_t rgbaToGray(uint8_t r, uint8_t g, uint8_t b) {
    int y = (77 * r + 150 * g + 29 * b + 128) >> 8;
    return (uint8_t)std::clamp(y, 0, 255);
}

mirage::Result<void> TemplateStore::loadFromFile(
    int template_id,
    const std::string& path_utf8)
{
    if (template_id < 0) {
        return mirage::Err<void>("template_id<0");
    }

    int w = 0, h = 0, channels = 0;
    unsigned char* img = nullptr;

    if (cfg_.prefer_gray8) {
        // Gray8として直接読み込み
        img = stbi_load(path_utf8.c_str(), &w, &h, &channels, 1);
    }
    if (!img) {
        // RGBA フォールバック → Gray8変換
        img = stbi_load(path_utf8.c_str(), &w, &h, &channels, 4);
        if (!img) {
            std::string err = "stbi_load失敗: " + path_utf8;
            MLOG_ERROR(TAG, "%s", err.c_str());
            return mirage::Err<void>(err);
        }

        // RGBA → Gray8
        std::vector<uint8_t> gray((size_t)w * h);
        for (int i = 0; i < w * h; i++) {
            gray[i] = rgbaToGray(img[i * 4 + 0], img[i * 4 + 1], img[i * 4 + 2]);
        }
        stbi_image_free(img);

        TemplateHandle th;
        th.template_id = template_id;
        th.w = w;
        th.h = h;
        th.gray_data = std::move(gray);
        th.source_path_utf8 = path_utf8;
        th.debug = "loaded(rgba->gray)";
        map_[template_id] = std::move(th);

        MLOG_DEBUG(TAG, "テンプレート読込(RGBA->Gray): id=%d %dx%d %s",
                   template_id, w, h, path_utf8.c_str());
        return mirage::Ok();
    }

    // Gray8直接読込成功
    TemplateHandle th;
    th.template_id = template_id;
    th.w = w;
    th.h = h;
    th.gray_data.assign(img, img + (size_t)w * h);
    th.source_path_utf8 = path_utf8;
    th.debug = "loaded(gray8)";
    stbi_image_free(img);

    map_[template_id] = std::move(th);

    MLOG_DEBUG(TAG, "テンプレート読込(Gray8): id=%d %dx%d %s",
               template_id, w, h, path_utf8.c_str());
    return mirage::Ok();
}

mirage::Result<void> TemplateStore::registerGray8(
    int template_id,
    const uint8_t* gray_data,
    int w, int h,
    const std::string& src_path_utf8)
{
    if (template_id < 0) return mirage::Err<void>("template_id<0");
    if (!gray_data) return mirage::Err<void>("gray_data=null");
    if (w <= 0 || h <= 0) return mirage::Err<void>("invalid size");

    TemplateHandle th;
    th.template_id = template_id;
    th.w = w;
    th.h = h;
    th.gray_data.assign(gray_data, gray_data + (size_t)w * h);
    th.source_path_utf8 = src_path_utf8;
    th.debug = "registered";
    map_[template_id] = std::move(th);

    MLOG_DEBUG(TAG, "テンプレート登録: id=%d %dx%d", template_id, w, h);
    return mirage::Ok();
}

const TemplateHandle* TemplateStore::get(int template_id) const {
    auto it = map_.find(template_id);
    if (it == map_.end()) return nullptr;
    return &it->second;
}

std::vector<int> TemplateStore::listTemplateIds() const {
    std::vector<int> ids;
    ids.reserve(map_.size());
    for (auto& kv : map_) ids.push_back(kv.first);
    return ids;
}

void TemplateStore::clear() { map_.clear(); }
void TemplateStore::remove(int template_id) { map_.erase(template_id); }

} // namespace mirage::ai
