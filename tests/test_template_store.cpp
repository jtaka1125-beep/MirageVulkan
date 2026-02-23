// =============================================================================
// Unit tests for TemplateStore CRUD operations (src/ai/template_store.hpp)
// Covers: registerGray8, get, listTemplateIds, remove, clear, size, error paths
// Complements test_template_versioning.cpp (version/changelog) and
// test_ai_e2e.cpp (integration with AIEngine).
// =============================================================================
#include <gtest/gtest.h>
#include <algorithm>
#include "ai/template_store.hpp"

using namespace mirage::ai;

// ---------------------------------------------------------------------------
// Helper: generate a flat Gray8 image (all pixels = value)
// ---------------------------------------------------------------------------
static std::vector<uint8_t> makeGray(int w, int h, uint8_t value = 128) {
    return std::vector<uint8_t>(static_cast<size_t>(w) * h, value);
}

// ---------------------------------------------------------------------------
// TS-1: Empty store — size/get/listTemplateIds
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, EmptyStoreDefaults) {
    TemplateStore store;
    EXPECT_EQ(store.size(), 0u);
    EXPECT_EQ(store.get(0), nullptr);
    EXPECT_EQ(store.get(999), nullptr);
    EXPECT_TRUE(store.listTemplateIds().empty());
}

// ---------------------------------------------------------------------------
// TS-2: registerGray8 valid data → size=1, handle retrievable
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, RegisterGray8_Valid) {
    TemplateStore store;
    auto g = makeGray(8, 8, 200);
    auto r = store.registerGray8(1, g.data(), 8, 8, "test.png");
    EXPECT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(store.size(), 1u);
    const TemplateHandle* h = store.get(1);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->w, 8);
    EXPECT_EQ(h->h, 8);
    EXPECT_EQ(h->template_id, 1);
}

// ---------------------------------------------------------------------------
// TS-3: registerGray8 w=0 → Err
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, RegisterGray8_ZeroWidth) {
    TemplateStore store;
    auto g = makeGray(1, 1);
    auto r = store.registerGray8(1, g.data(), 0, 8, "");
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(store.size(), 0u);
}

// ---------------------------------------------------------------------------
// TS-4: registerGray8 h=0 → Err
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, RegisterGray8_ZeroHeight) {
    TemplateStore store;
    auto g = makeGray(1, 1);
    auto r = store.registerGray8(1, g.data(), 8, 0, "");
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(store.size(), 0u);
}

// ---------------------------------------------------------------------------
// TS-5: registerGray8 null data → Err
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, RegisterGray8_NullData) {
    TemplateStore store;
    auto r = store.registerGray8(1, nullptr, 8, 8, "");
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(store.size(), 0u);
}

// ---------------------------------------------------------------------------
// TS-6: listTemplateIds returns all registered IDs (no duplicates)
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, ListTemplateIds_Multiple) {
    TemplateStore store;
    auto g = makeGray(4, 4);
    store.registerGray8(10, g.data(), 4, 4, "");
    store.registerGray8(20, g.data(), 4, 4, "");
    store.registerGray8(30, g.data(), 4, 4, "");

    auto ids = store.listTemplateIds();
    ASSERT_EQ(ids.size(), 3u);
    // Sort for deterministic comparison
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 20);
    EXPECT_EQ(ids[2], 30);
}

// ---------------------------------------------------------------------------
// TS-7: remove existing entry → size decrements, get returns nullptr
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, Remove_Existing) {
    TemplateStore store;
    auto g = makeGray(4, 4);
    store.registerGray8(5, g.data(), 4, 4, "");
    EXPECT_EQ(store.size(), 1u);

    store.remove(5);
    EXPECT_EQ(store.size(), 0u);
    EXPECT_EQ(store.get(5), nullptr);
    EXPECT_TRUE(store.listTemplateIds().empty());
}

// ---------------------------------------------------------------------------
// TS-8: remove non-existent ID → no crash, size unchanged
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, Remove_NonExistent) {
    TemplateStore store;
    auto g = makeGray(4, 4);
    store.registerGray8(5, g.data(), 4, 4, "");
    EXPECT_EQ(store.size(), 1u);

    EXPECT_NO_THROW(store.remove(999));  // non-existent
    EXPECT_EQ(store.size(), 1u);        // unchanged
}

// ---------------------------------------------------------------------------
// TS-9: clear → size=0, listTemplateIds empty
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, Clear_AllRemoved) {
    TemplateStore store;
    auto g = makeGray(4, 4);
    store.registerGray8(1, g.data(), 4, 4, "");
    store.registerGray8(2, g.data(), 4, 4, "");
    store.registerGray8(3, g.data(), 4, 4, "");
    EXPECT_EQ(store.size(), 3u);

    store.clear();
    EXPECT_EQ(store.size(), 0u);
    EXPECT_TRUE(store.listTemplateIds().empty());
    EXPECT_EQ(store.get(1), nullptr);
}

// ---------------------------------------------------------------------------
// TS-10: gray_data in handle matches registered input exactly
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, Gray8DataPreserved) {
    TemplateStore store;
    // Gradient: pixels are 0, 1, 2, ..., 255 (mod 256)
    std::vector<uint8_t> g(16 * 16);
    for (int i = 0; i < 256; ++i) g[i] = static_cast<uint8_t>(i);

    store.registerGray8(42, g.data(), 16, 16, "gradient.png");
    const TemplateHandle* h = store.get(42);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(h->gray_data.size(), 256u);
    EXPECT_EQ(h->gray_data, g);  // pixel-by-pixel comparison
}

// ---------------------------------------------------------------------------
// TS-11: source_path_utf8 is stored in handle
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, SourcePathPreserved) {
    TemplateStore store;
    auto g = makeGray(4, 4);
    store.registerGray8(7, g.data(), 4, 4, "C:/templates/btn_ok.png");
    const TemplateHandle* h = store.get(7);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->source_path_utf8, "C:/templates/btn_ok.png");
}

// ---------------------------------------------------------------------------
// TS-12: Registering same ID twice keeps size=1 (update, not duplicate)
// ---------------------------------------------------------------------------
TEST(TemplateStoreTest, RegisterSameIdTwice_NoDuplicate) {
    TemplateStore store;
    auto g1 = makeGray(4, 4, 100);
    auto g2 = makeGray(4, 4, 200);  // different content

    store.registerGray8(99, g1.data(), 4, 4, "v1.png");
    EXPECT_EQ(store.size(), 1u);

    store.registerGray8(99, g2.data(), 4, 4, "v2.png");  // overwrite
    EXPECT_EQ(store.size(), 1u);  // still 1, not 2

    auto ids = store.listTemplateIds();
    EXPECT_EQ(ids.size(), 1u);

    // Newest data should be in the store
    const TemplateHandle* h = store.get(99);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->gray_data, g2);
}
