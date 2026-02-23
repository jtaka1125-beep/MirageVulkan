// =============================================================================
// Unit tests for TemplateStore versioning (改善P)
// GPU不要 — checksum/version管理のCPU純粋ロジックテスト
// =============================================================================
#include <gtest/gtest.h>
#include "ai/template_store.hpp"

using namespace mirage::ai;

static std::vector<uint8_t> makeGray(int w, int h, uint8_t fill = 128) {
    return std::vector<uint8_t>((size_t)w * h, fill);
}

TEST(TemplateVersioningTest, InitialVersionIsOne) {
    TemplateStore store;
    auto g = makeGray(4, 4);
    ASSERT_TRUE(store.registerGray8(1, g.data(), 4, 4, "test").is_ok());
    const auto* h = store.get(1);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->version, 1);
    EXPECT_NE(h->checksum, 0u);
    EXPECT_FALSE(h->added_at.empty());
    EXPECT_FALSE(h->updated_at.empty());
}

TEST(TemplateVersioningTest, SameDataNoVersionIncrement) {
    TemplateStore store;
    auto g = makeGray(4, 4, 100);
    ASSERT_TRUE(store.registerGray8(2, g.data(), 4, 4, "t").is_ok());
    int v1 = store.get(2)->version;
    ASSERT_TRUE(store.registerGray8(2, g.data(), 4, 4, "t").is_ok());
    EXPECT_EQ(v1, store.get(2)->version);
}

TEST(TemplateVersioningTest, ChangedDataIncrementsVersion) {
    TemplateStore store;
    auto g1 = makeGray(4, 4, 50);
    auto g2 = makeGray(4, 4, 200);
    ASSERT_TRUE(store.registerGray8(3, g1.data(), 4, 4, "t").is_ok());
    EXPECT_EQ(store.getTemplateVersion(3), 1);
    ASSERT_TRUE(store.registerGray8(3, g2.data(), 4, 4, "t").is_ok());
    EXPECT_EQ(store.getTemplateVersion(3), 2);
    ASSERT_TRUE(store.registerGray8(3, g1.data(), 4, 4, "t").is_ok());
    EXPECT_EQ(store.getTemplateVersion(3), 3);
}

TEST(TemplateVersioningTest, AddedAtIsPreserved) {
    TemplateStore store;
    auto g1 = makeGray(4, 4, 10);
    auto g2 = makeGray(4, 4, 20);
    ASSERT_TRUE(store.registerGray8(4, g1.data(), 4, 4, "t").is_ok());
    std::string added = store.get(4)->added_at;
    ASSERT_TRUE(store.registerGray8(4, g2.data(), 4, 4, "t").is_ok());
    EXPECT_EQ(store.get(4)->added_at, added);
    EXPECT_FALSE(store.get(4)->updated_at.empty());
}

TEST(TemplateVersioningTest, ChecksumDiffersForDiffData) {
    TemplateStore store;
    auto g1 = makeGray(4, 4, 0);
    auto g2 = makeGray(4, 4, 255);
    store.registerGray8(10, g1.data(), 4, 4, "");
    store.registerGray8(11, g2.data(), 4, 4, "");
    EXPECT_NE(store.get(10)->checksum, store.get(11)->checksum);
}

TEST(TemplateVersioningTest, ChangeLogRecordsAdded) {
    TemplateStore store;
    auto g = makeGray(4, 4);
    store.registerGray8(20, g.data(), 4, 4, "");
    const auto& log = store.getChangeLogs();
    ASSERT_FALSE(log.empty());
    EXPECT_EQ(log.back().template_id, 20);
    EXPECT_EQ(log.back().version, 1);
    EXPECT_EQ(log.back().event, "added");
}

TEST(TemplateVersioningTest, ChangeLogRecordsUpdated) {
    TemplateStore store;
    auto g1 = makeGray(4, 4, 1);
    auto g2 = makeGray(4, 4, 2);
    store.registerGray8(21, g1.data(), 4, 4, "");
    store.registerGray8(21, g2.data(), 4, 4, "");
    const auto& log = store.getChangeLogs();
    ASSERT_GE(log.size(), 2u);
    EXPECT_EQ(log.back().event, "updated");
    EXPECT_EQ(log.back().version, 2);
}

TEST(TemplateVersioningTest, GetVersionUnregistered) {
    TemplateStore store;
    EXPECT_EQ(store.getTemplateVersion(999), 0);
}

TEST(TemplateVersioningTest, ChangeLogCapAt200) {
    TemplateStore store;
    for (int i = 0; i < 210; ++i) {
        auto g = makeGray(2, 2, (uint8_t)(i & 0xFF));
        store.registerGray8(i, g.data(), 2, 2, "");
    }
    EXPECT_LE(store.getChangeLogs().size(), 200u);
}
