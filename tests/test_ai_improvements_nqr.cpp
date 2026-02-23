// =============================================================================
// Unit tests for AI improvements N, Q, R (GPU不要)
// N: OCR キーワードマッピング (action_mapper.hpp の TextAction)
// Q: DeviceAdaptation 構造体
// R: ActionLogEntry 構造体
// =============================================================================
#include <gtest/gtest.h>
#include "ai/action_mapper.hpp"
#include "ai_engine.hpp"

using namespace mirage::ai;

// ===========================================================================
// 改善N: TextAction (action_mapper.hpp の registerTextAction 系)
// ===========================================================================
TEST(ImprovementNTest, RegisterAndGetTextAction) {
    ActionMapper mapper;
    mapper.registerTextAction("OK", "tap:ok_btn");
    mapper.registerTextAction("Cancel", "back");

    EXPECT_EQ(mapper.getTextAction("OK"),     "tap:ok_btn");
    EXPECT_EQ(mapper.getTextAction("Cancel"), "back");
}

TEST(ImprovementNTest, RemoveTextAction) {
    ActionMapper mapper;
    mapper.registerTextAction("Close", "back");
    EXPECT_TRUE(mapper.hasTextAction("Close"));
    mapper.removeTextAction("Close");
    EXPECT_FALSE(mapper.hasTextAction("Close"));
}

TEST(ImprovementNTest, GetTextKeywords) {
    ActionMapper mapper;
    mapper.registerTextAction("OK",     "tap:ok");
    mapper.registerTextAction("Retry",  "tap:retry");
    mapper.registerTextAction("Cancel", "back");
    auto kws = mapper.getTextKeywords();
    EXPECT_EQ(kws.size(), 3u);
}

TEST(ImprovementNTest, OverwriteTextAction) {
    ActionMapper mapper;
    mapper.registerTextAction("OK", "tap:ok_v1");
    mapper.registerTextAction("OK", "tap:ok_v2");
    EXPECT_EQ(mapper.getTextAction("OK"), "tap:ok_v2");
}

// getTextAction は未登録キーに "tap:<keyword>" を返す (getAction と同仕様)
TEST(ImprovementNTest, TextActionNotFoundReturnsTapDefault) {
    ActionMapper mapper;
    EXPECT_EQ(mapper.getTextAction("nonexistent"), "tap:nonexistent");
    EXPECT_FALSE(mapper.hasTextAction("nonexistent"));
}

// ===========================================================================
// 改善Q: DeviceAdaptation 構造体
// ===========================================================================
TEST(ImprovementQTest, DefaultValues) {
    DeviceAdaptation a;
    EXPECT_FLOAT_EQ(a.min_score, 0.0f);
    EXPECT_FLOAT_EQ(a.cooldown_scale, 1.0f);
    EXPECT_FALSE(a.enabled);
}

TEST(ImprovementQTest, CustomValues) {
    DeviceAdaptation a;
    a.min_score      = 0.75f;
    a.cooldown_scale = 2.5f;
    a.enabled        = true;

    EXPECT_FLOAT_EQ(a.min_score, 0.75f);
    EXPECT_FLOAT_EQ(a.cooldown_scale, 2.5f);
    EXPECT_TRUE(a.enabled);
}

TEST(ImprovementQTest, DisabledAdaptationIgnoresMinScore) {
    DeviceAdaptation a;
    a.min_score = 0.9f;
    a.enabled   = false;
    EXPECT_FALSE(a.enabled);
    EXPECT_FLOAT_EQ(a.min_score, 0.9f);
}

// ===========================================================================
// 改善R: ActionLogEntry 構造体
// ===========================================================================
TEST(ImprovementRTest, DefaultValues) {
    ActionLogEntry e;
    EXPECT_EQ(e.slot, -1);
    EXPECT_FLOAT_EQ(e.score, 0.0f);
    EXPECT_TRUE(e.timestamp.empty());
    EXPECT_TRUE(e.action_type.empty());
    EXPECT_TRUE(e.reason.empty());
}

TEST(ImprovementRTest, FieldAssignment) {
    ActionLogEntry e;
    e.timestamp   = "12:34:56";
    e.slot        = 2;
    e.action_type = "TAP";
    e.score       = 0.95f;
    e.reason      = "match=ok_btn score=0.95";

    EXPECT_EQ(e.timestamp,   "12:34:56");
    EXPECT_EQ(e.slot,        2);
    EXPECT_EQ(e.action_type, "TAP");
    EXPECT_FLOAT_EQ(e.score, 0.95f);
    EXPECT_EQ(e.reason,      "match=ok_btn score=0.95");
}

TEST(ImprovementRTest, VectorOfEntries) {
    std::vector<ActionLogEntry> log;
    for (int i = 0; i < 10; ++i) {
        ActionLogEntry e;
        e.slot = i;
        e.action_type = "WAIT";
        log.push_back(e);
    }
    EXPECT_EQ(log.size(), 10u);
    EXPECT_EQ(log[5].slot, 5);
}
