// =============================================================================
// Unit tests for ActionMapper (src/ai/action_mapper.hpp)
// Covers: addTemplateAction, getAction, hasAction, removeTemplateAction,
//         registerTextAction, classifyState, clear, size
// AM-1 through AM-14
// =============================================================================
#include <gtest/gtest.h>
#include "ai/action_mapper.hpp"

using namespace mirage::ai;

// ---------------------------------------------------------------------------
// AM-1: Empty mapper — size=0, hasAction=false, getAction returns "tap:<id>"
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, EmptyMapper) {
    ActionMapper m;
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.textActionSize(), 0u);
    EXPECT_FALSE(m.hasAction("foo"));
    EXPECT_EQ(m.getAction("foo"), "tap:foo");  // default: "tap:<id>"
}

// ---------------------------------------------------------------------------
// AM-2: addTemplateAction + hasAction + getAction
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, AddAndGetTemplateAction) {
    ActionMapper m;
    m.addTemplateAction("btn_ok", "tap_ok");
    EXPECT_TRUE(m.hasAction("btn_ok"));
    EXPECT_EQ(m.getAction("btn_ok"), "tap_ok");
    EXPECT_EQ(m.size(), 1u);
}

// ---------------------------------------------------------------------------
// AM-3: getAction for unknown key returns default "tap:<id>"
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, GetActionUnknown) {
    ActionMapper m;
    m.addTemplateAction("btn_ok", "tap_ok");
    EXPECT_EQ(m.getAction("btn_cancel"), "tap:btn_cancel");  // default fallback
    EXPECT_FALSE(m.hasAction("btn_cancel"));
}

// ---------------------------------------------------------------------------
// AM-4: addTemplateAction overwrite same key
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, OverwriteExistingAction) {
    ActionMapper m;
    m.addTemplateAction("btn_ok", "tap_ok");
    m.addTemplateAction("btn_ok", "double_tap");
    EXPECT_EQ(m.getAction("btn_ok"), "double_tap");
    EXPECT_EQ(m.size(), 1u);  // still 1, not 2
}

// ---------------------------------------------------------------------------
// AM-5: removeTemplateAction existing key
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, RemoveExistingAction) {
    ActionMapper m;
    m.addTemplateAction("btn_ok", "tap_ok");
    m.removeTemplateAction("btn_ok");
    EXPECT_FALSE(m.hasAction("btn_ok"));
    EXPECT_EQ(m.size(), 0u);
}

// ---------------------------------------------------------------------------
// AM-6: removeTemplateAction non-existent key — no crash
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, RemoveNonExistentAction) {
    ActionMapper m;
    m.addTemplateAction("btn_ok", "tap_ok");
    EXPECT_NO_THROW(m.removeTemplateAction("btn_cancel"));
    EXPECT_EQ(m.size(), 1u);
}

// ---------------------------------------------------------------------------
// AM-7: multiple template actions
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, MultipleTemplateActions) {
    ActionMapper m;
    m.addTemplateAction("btn_a", "action_a");
    m.addTemplateAction("btn_b", "action_b");
    m.addTemplateAction("btn_c", "action_c");
    EXPECT_EQ(m.size(), 3u);
    EXPECT_EQ(m.getAction("btn_a"), "action_a");
    EXPECT_EQ(m.getAction("btn_b"), "action_b");
    EXPECT_EQ(m.getAction("btn_c"), "action_c");
}

// ---------------------------------------------------------------------------
// AM-8: registerTextAction + hasTextAction + getTextAction
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, RegisterAndGetTextAction) {
    ActionMapper m;
    m.registerTextAction("loading", "wait");
    EXPECT_TRUE(m.hasTextAction("loading"));
    EXPECT_EQ(m.getTextAction("loading"), "wait");
    EXPECT_EQ(m.textActionSize(), 1u);
}

// ---------------------------------------------------------------------------
// AM-9: getTextAction unknown keyword returns "tap:<keyword>"
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, GetTextActionUnknown) {
    ActionMapper m;
    m.registerTextAction("loading", "wait");
    EXPECT_EQ(m.getTextAction("error"), "tap:error");  // default fallback
    EXPECT_FALSE(m.hasTextAction("error"));
}

// ---------------------------------------------------------------------------
// AM-10: removeTextAction
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, RemoveTextAction) {
    ActionMapper m;
    m.registerTextAction("loading", "wait");
    m.removeTextAction("loading");
    EXPECT_FALSE(m.hasTextAction("loading"));
    EXPECT_EQ(m.textActionSize(), 0u);
}

// ---------------------------------------------------------------------------
// AM-11: clear() wipes both template and text actions
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClearWipesBoth) {
    ActionMapper m;
    m.addTemplateAction("btn_ok", "tap_ok");
    m.registerTextAction("loading", "wait");
    m.clear();
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.textActionSize(), 0u);
    EXPECT_FALSE(m.hasAction("btn_ok"));
    EXPECT_FALSE(m.hasTextAction("loading"));
}

// ---------------------------------------------------------------------------
// AM-12: classifyState — no matches → NORMAL
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClassifyState_Empty_IsNormal) {
    ActionMapper m;
    std::vector<MatchResultLite> empty;
    EXPECT_EQ(m.classifyState(empty), ActionMapper::ScreenState::NORMAL);
}

// ---------------------------------------------------------------------------
// AM-13: classifyState — "loading" in name → LOADING
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClassifyState_Loading) {
    ActionMapper m;
    std::vector<MatchResultLite> matches = {
        {1, "btn_ok"},
        {2, "loading_spinner"}
    };
    EXPECT_EQ(m.classifyState(matches), ActionMapper::ScreenState::LOADING);
}

// ---------------------------------------------------------------------------
// AM-14: classifyState — "error" in name → ERROR_POPUP
// ---------------------------------------------------------------------------
TEST(ActionMapperTest, ClassifyState_Error) {
    ActionMapper m;
    std::vector<MatchResultLite> matches = {
        {1, "error_dialog"},
        {2, "btn_ok"}
    };
    EXPECT_EQ(m.classifyState(matches), ActionMapper::ScreenState::ERROR_POPUP);
}
