#pragma once
// =============================================================================
// ActionMapper — テンプレートIDからアクション文字列を決定
// ai_engine.cpp から抽出（テスト可能化）
// =============================================================================
#include <string>
#include <unordered_map>
#include <vector>

namespace mirage::ai {

// VkMatchResult のテスト用軽量コピー（Vulkan依存を避ける）
struct MatchResultLite {
    int template_id = 0;
    std::string name;  // classifyState用
};

class ActionMapper {
public:
    void addTemplateAction(const std::string& template_id, const std::string& action) {
        actions_[template_id] = action;
    }

    void removeTemplateAction(const std::string& template_id) {
        actions_.erase(template_id);
    }

    bool hasAction(const std::string& template_id) const {
        return actions_.count(template_id) > 0;
    }

    // テンプレート名からアクションを取得（未登録なら "tap:<name>"）
    std::string getAction(const std::string& template_id) const {
        auto it = actions_.find(template_id);
        if (it != actions_.end()) return it->second;
        return "tap:" + template_id;
    }

    // マッチ結果の分類（loading/errorの判定）
    enum class ScreenState { NORMAL, LOADING, ERROR_POPUP };

    ScreenState classifyState(const std::vector<MatchResultLite>& matches) const {
        for (const auto& m : matches) {
            const auto& name = m.name;
            if (name.find("loading") != std::string::npos ||
                name.find("spinner") != std::string::npos) {
                return ScreenState::LOADING;
            }
            if (name.find("error") != std::string::npos ||
                name.find("popup") != std::string::npos) {
                return ScreenState::ERROR_POPUP;
            }
        }
        return ScreenState::NORMAL;
    }

    // =========================================================================
    // テキストアクションマッピング（OCRキーワード → アクション）
    // =========================================================================

    // OCRキーワードに対するアクション登録（大文字小文字無視で検索される）
    void registerTextAction(const std::string& keyword, const std::string& action) {
        text_actions_[keyword] = action;
    }

    void removeTextAction(const std::string& keyword) {
        text_actions_.erase(keyword);
    }

    bool hasTextAction(const std::string& keyword) const {
        return text_actions_.count(keyword) > 0;
    }

    // キーワードからアクション文字列を取得（未登録なら "tap:<keyword>"）
    std::string getTextAction(const std::string& keyword) const {
        auto it = text_actions_.find(keyword);
        if (it != text_actions_.end()) return it->second;
        return "tap:" + keyword;
    }

    // 登録済みキーワード一覧を返す（OCR検索用）
    std::vector<std::string> getTextKeywords() const {
        std::vector<std::string> keys;
        keys.reserve(text_actions_.size());
        for (const auto& pair : text_actions_) {
            keys.push_back(pair.first);
        }
        return keys;
    }

    size_t textActionSize() const { return text_actions_.size(); }

    size_t size() const { return actions_.size(); }

    void clear() {
        actions_.clear();
        text_actions_.clear();
    }

private:
    std::unordered_map<std::string, std::string> actions_;
    std::unordered_map<std::string, std::string> text_actions_;  // keyword → action
};

} // namespace mirage::ai
