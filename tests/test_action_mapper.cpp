// =============================================================================
// Unit tests for ActionMapper (src/ai/action_mapper.hpp)
// GPU不要 — テンプレートID→アクション決定のCPU純粋ロジックテスト
// =============================================================================
#include <gtest/gtest.h>
#include <algorithm>
#include "ai/action_mapper.hpp"

using namespace mirage::ai;

// ---------------------------------------------------------------------------
// 未登録テンプレート → デフォルト "tap:<name>"
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, DefaultAction) {
    ActionMapper mapper;
    EXPECT_EQ(mapper.getAction("button_ok"), "tap:button_ok");
    EXPECT_EQ(mapper.getAction("unknown"), "tap:unknown");
}

// ---------------------------------------------------------------------------
// アクション登録 → 取得
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, RegisterAndGet) {
    ActionMapper mapper;
    mapper.addTemplateAction("close_btn", "back");
    mapper.addTemplateAction("next_btn", "tap:next_btn");
    mapper.addTemplateAction("scroll_down", "swipe:down");

    EXPECT_EQ(mapper.getAction("close_btn"), "back");
    EXPECT_EQ(mapper.getAction("next_btn"), "tap:next_btn");
    EXPECT_EQ(mapper.getAction("scroll_down"), "swipe:down");
}

// ---------------------------------------------------------------------------
// アクション上書き
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, OverwriteAction) {
    ActionMapper mapper;
    mapper.addTemplateAction("btn", "tap:btn");
    EXPECT_EQ(mapper.getAction("btn"), "tap:btn");

    mapper.addTemplateAction("btn", "back");
    EXPECT_EQ(mapper.getAction("btn"), "back");
}

// ---------------------------------------------------------------------------
// hasAction
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, HasAction) {
    ActionMapper mapper;
    EXPECT_FALSE(mapper.hasAction("btn"));
    mapper.addTemplateAction("btn", "tap:btn");
    EXPECT_TRUE(mapper.hasAction("btn"));
}

// ---------------------------------------------------------------------------
// removeTemplateAction
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, RemoveAction) {
    ActionMapper mapper;
    mapper.addTemplateAction("btn", "tap:btn");
    EXPECT_TRUE(mapper.hasAction("btn"));

    mapper.removeTemplateAction("btn");
    EXPECT_FALSE(mapper.hasAction("btn"));
    // 削除後はデフォルトに戻る
    EXPECT_EQ(mapper.getAction("btn"), "tap:btn");
}

// ---------------------------------------------------------------------------
// size / clear
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, SizeAndClear) {
    ActionMapper mapper;
    EXPECT_EQ(mapper.size(), 0u);

    mapper.addTemplateAction("a", "tap:a");
    mapper.addTemplateAction("b", "tap:b");
    mapper.addTemplateAction("c", "back");
    EXPECT_EQ(mapper.size(), 3u);

    mapper.clear();
    EXPECT_EQ(mapper.size(), 0u);
}

// ---------------------------------------------------------------------------
// 存在しないキーのremoveは安全
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, RemoveNonExistent) {
    ActionMapper mapper;
    mapper.removeTemplateAction("nonexistent");  // クラッシュしないこと
    EXPECT_EQ(mapper.size(), 0u);
}

// ---------------------------------------------------------------------------
// classifyState — NORMAL（マッチなし）
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClassifyState_NoMatches) {
    ActionMapper mapper;
    std::vector<MatchResultLite> matches;
    EXPECT_EQ(mapper.classifyState(matches), ActionMapper::ScreenState::NORMAL);
}

// ---------------------------------------------------------------------------
// classifyState — NORMAL（通常テンプレート）
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClassifyState_Normal) {
    ActionMapper mapper;
    std::vector<MatchResultLite> matches;
    matches.push_back({1, "button_ok"});
    matches.push_back({2, "icon_close"});

    EXPECT_EQ(mapper.classifyState(matches), ActionMapper::ScreenState::NORMAL);
}

// ---------------------------------------------------------------------------
// classifyState — LOADING検出
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClassifyState_Loading) {
    ActionMapper mapper;

    // "loading"キーワード
    {
        std::vector<MatchResultLite> matches;
        matches.push_back({1, "screen_loading"});
        EXPECT_EQ(mapper.classifyState(matches), ActionMapper::ScreenState::LOADING);
    }

    // "spinner"キーワード
    {
        std::vector<MatchResultLite> matches;
        matches.push_back({1, "progress_spinner_icon"});
        EXPECT_EQ(mapper.classifyState(matches), ActionMapper::ScreenState::LOADING);
    }
}

// ---------------------------------------------------------------------------
// classifyState — ERROR_POPUP検出
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClassifyState_ErrorPopup) {
    ActionMapper mapper;

    // "error"キーワード
    {
        std::vector<MatchResultLite> matches;
        matches.push_back({1, "dialog_error"});
        EXPECT_EQ(mapper.classifyState(matches), ActionMapper::ScreenState::ERROR_POPUP);
    }

    // "popup"キーワード
    {
        std::vector<MatchResultLite> matches;
        matches.push_back({1, "notification_popup"});
        EXPECT_EQ(mapper.classifyState(matches), ActionMapper::ScreenState::ERROR_POPUP);
    }
}

// ---------------------------------------------------------------------------
// classifyState — LOADING優先（LOADINGとERROR両方ある場合）
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClassifyState_LoadingPriority) {
    ActionMapper mapper;
    std::vector<MatchResultLite> matches;
    matches.push_back({1, "loading_indicator"});
    matches.push_back({2, "error_dialog"});

    // LOADINGが先にマッチ → LOADING
    EXPECT_EQ(mapper.classifyState(matches), ActionMapper::ScreenState::LOADING);
}

// ---------------------------------------------------------------------------
// classifyState — 名前なしエントリは無視
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClassifyState_EmptyName) {
    ActionMapper mapper;
    std::vector<MatchResultLite> matches;
    matches.push_back({1, ""});
    matches.push_back({2, "normal_button"});

    EXPECT_EQ(mapper.classifyState(matches), ActionMapper::ScreenState::NORMAL);
}

// ---------------------------------------------------------------------------
// 大量のテンプレートアクション登録
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ManyActions) {
    ActionMapper mapper;
    const int N = 500;
    for (int i = 0; i < N; ++i) {
        std::string id = "tpl_" + std::to_string(i);
        mapper.addTemplateAction(id, "tap:" + id);
    }
    EXPECT_EQ(mapper.size(), (size_t)N);

    // 全取得確認
    for (int i = 0; i < N; ++i) {
        std::string id = "tpl_" + std::to_string(i);
        EXPECT_EQ(mapper.getAction(id), "tap:" + id);
    }
}

// ---------------------------------------------------------------------------
// 空文字テンプレートID
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, EmptyTemplateId) {
    ActionMapper mapper;
    mapper.addTemplateAction("", "tap:empty");
    EXPECT_EQ(mapper.getAction(""), "tap:empty");
    EXPECT_TRUE(mapper.hasAction(""));
}

// ===========================================================================
// TextActionMapping テスト（OCRキーワード → アクション）
// ===========================================================================

// ---------------------------------------------------------------------------
// 未登録キーワード → デフォルト "tap:<keyword>"
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, TextAction_Default) {
    ActionMapper mapper;
    EXPECT_EQ(mapper.getTextAction("OK"), "tap:OK");
    EXPECT_EQ(mapper.getTextAction("Cancel"), "tap:Cancel");
}

// ---------------------------------------------------------------------------
// テキストアクション登録 → 取得
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, TextAction_RegisterAndGet) {
    ActionMapper mapper;
    mapper.registerTextAction("OK", "tap:ok_button");
    mapper.registerTextAction("Cancel", "back");
    mapper.registerTextAction("Next", "tap:next");

    EXPECT_EQ(mapper.getTextAction("OK"), "tap:ok_button");
    EXPECT_EQ(mapper.getTextAction("Cancel"), "back");
    EXPECT_EQ(mapper.getTextAction("Next"), "tap:next");
}

// ---------------------------------------------------------------------------
// hasTextAction / removeTextAction
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, TextAction_HasAndRemove) {
    ActionMapper mapper;
    EXPECT_FALSE(mapper.hasTextAction("OK"));

    mapper.registerTextAction("OK", "tap:ok");
    EXPECT_TRUE(mapper.hasTextAction("OK"));

    mapper.removeTextAction("OK");
    EXPECT_FALSE(mapper.hasTextAction("OK"));
    // 削除後はデフォルトに戻る
    EXPECT_EQ(mapper.getTextAction("OK"), "tap:OK");
}

// ---------------------------------------------------------------------------
// textActionSize
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, TextAction_Size) {
    ActionMapper mapper;
    EXPECT_EQ(mapper.textActionSize(), 0u);

    mapper.registerTextAction("OK", "tap:ok");
    mapper.registerTextAction("Cancel", "back");
    EXPECT_EQ(mapper.textActionSize(), 2u);
}

// ---------------------------------------------------------------------------
// getTextKeywords — 登録済みキーワード一覧
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, TextAction_GetKeywords) {
    ActionMapper mapper;
    auto keys = mapper.getTextKeywords();
    EXPECT_TRUE(keys.empty());

    mapper.registerTextAction("OK", "tap:ok");
    mapper.registerTextAction("Cancel", "back");
    mapper.registerTextAction("Retry", "tap:retry");

    keys = mapper.getTextKeywords();
    EXPECT_EQ(keys.size(), 3u);

    // 全キーワードが含まれていることを確認（順序不問）
    std::sort(keys.begin(), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), std::string("Cancel")), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), std::string("OK")), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), std::string("Retry")), keys.end());
}

// ---------------------------------------------------------------------------
// clear() はテキストアクションもクリアする
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, TextAction_ClearAll) {
    ActionMapper mapper;
    mapper.addTemplateAction("btn", "tap:btn");
    mapper.registerTextAction("OK", "tap:ok");
    EXPECT_EQ(mapper.size(), 1u);
    EXPECT_EQ(mapper.textActionSize(), 1u);

    mapper.clear();
    EXPECT_EQ(mapper.size(), 0u);
    EXPECT_EQ(mapper.textActionSize(), 0u);
}

// ---------------------------------------------------------------------------
// テキストアクション上書き
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, TextAction_Overwrite) {
    ActionMapper mapper;
    mapper.registerTextAction("OK", "tap:ok");
    EXPECT_EQ(mapper.getTextAction("OK"), "tap:ok");

    mapper.registerTextAction("OK", "back");
    EXPECT_EQ(mapper.getTextAction("OK"), "back");
    EXPECT_EQ(mapper.textActionSize(), 1u);
}

// ---------------------------------------------------------------------------
// 存在しないキーワードのremoveは安全
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, TextAction_RemoveNonExistent) {
    ActionMapper mapper;
    mapper.removeTextAction("nonexistent");  // クラッシュしないこと
    EXPECT_EQ(mapper.textActionSize(), 0u);
}
