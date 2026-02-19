// =============================================================================
// Unit tests for TemplateManifest (src/ai/template_manifest.hpp/cpp)
// GPU不要 — CPU純粋ロジックテスト
// =============================================================================
#include <gtest/gtest.h>
#include "ai/template_manifest.hpp"

#include <fstream>
#include <filesystem>
#include <cstdio>

using namespace mirage::ai;
namespace fs = std::filesystem;

// テスト用一時ディレクトリヘルパー
class ManifestTest : public ::testing::Test {
protected:
    std::string temp_dir_;
    std::string manifest_path_;

    void SetUp() override {
        temp_dir_ = (fs::temp_directory_path() / "mirage_manifest_test").string();
        fs::create_directories(temp_dir_);
        manifest_path_ = temp_dir_ + "/manifest.json";
    }

    void TearDown() override {
        fs::remove_all(temp_dir_);
    }

    // テスト用マニフェストを作成
    TemplateManifest makeTestManifest() {
        TemplateManifest m;
        m.version = 2;
        m.root_dir = "templates";

        TemplateEntry e1;
        e1.template_id = 1;
        e1.name = "button_ok";
        e1.file = "button_ok.png";
        e1.w = 64;
        e1.h = 32;
        e1.mtime_utc = 1000000;
        e1.crc32 = 0xDEADBEEF;
        e1.tags = "ui,button";
        m.entries.push_back(e1);

        TemplateEntry e2;
        e2.template_id = 5;
        e2.name = "icon_close";
        e2.file = "icons/close.png";
        e2.w = 24;
        e2.h = 24;
        e2.mtime_utc = 2000000;
        e2.crc32 = 0xCAFEBABE;
        e2.tags = "ui,icon";
        m.entries.push_back(e2);

        return m;
    }
};

// ---------------------------------------------------------------------------
// 保存 → 読込ラウンドトリップ
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, SaveAndLoadRoundTrip) {
    auto m = makeTestManifest();

    std::string err;
    ASSERT_TRUE(saveManifestJson(manifest_path_, m, &err)) << err;

    TemplateManifest loaded;
    ASSERT_TRUE(loadManifestJson(manifest_path_, loaded, &err)) << err;

    EXPECT_EQ(loaded.version, 2);
    EXPECT_EQ(loaded.root_dir, "templates");
    ASSERT_EQ(loaded.entries.size(), 2u);

    EXPECT_EQ(loaded.entries[0].template_id, 1);
    EXPECT_EQ(loaded.entries[0].name, "button_ok");
    EXPECT_EQ(loaded.entries[0].file, "button_ok.png");
    EXPECT_EQ(loaded.entries[0].w, 64);
    EXPECT_EQ(loaded.entries[0].h, 32);
    EXPECT_EQ(loaded.entries[0].mtime_utc, 1000000u);
    EXPECT_EQ(loaded.entries[0].crc32, 0xDEADBEEFu);
    EXPECT_EQ(loaded.entries[0].tags, "ui,button");

    EXPECT_EQ(loaded.entries[1].template_id, 5);
    EXPECT_EQ(loaded.entries[1].name, "icon_close");
    EXPECT_EQ(loaded.entries[1].file, "icons/close.png");
    EXPECT_EQ(loaded.entries[1].w, 24);
    EXPECT_EQ(loaded.entries[1].h, 24);
}

// ---------------------------------------------------------------------------
// indexById
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, IndexById) {
    auto m = makeTestManifest();
    auto idx = indexById(m);

    ASSERT_EQ(idx.size(), 2u);
    EXPECT_EQ(idx.at(1), 0u);
    EXPECT_EQ(idx.at(5), 1u);
    EXPECT_EQ(idx.count(999), 0u);
}

// ---------------------------------------------------------------------------
// allocateNextId — 空マニフェスト
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, AllocateNextId_Empty) {
    TemplateManifest m;
    EXPECT_EQ(allocateNextId(m, 1), 1);
    EXPECT_EQ(allocateNextId(m, 100), 100);
}

// ---------------------------------------------------------------------------
// allocateNextId — ID=1,5が使用中 → 2が返る
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, AllocateNextId_SkipsUsed) {
    auto m = makeTestManifest();
    int next = allocateNextId(m, 1);
    EXPECT_EQ(next, 2);  // ID=1が使用中なので2
}

// ---------------------------------------------------------------------------
// allocateNextId — 連続ID
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, AllocateNextId_Consecutive) {
    TemplateManifest m;
    for (int i = 1; i <= 5; ++i) {
        TemplateEntry e;
        e.template_id = i;
        e.name = "tpl_" + std::to_string(i);
        m.entries.push_back(e);
    }
    int next = allocateNextId(m, 1);
    EXPECT_EQ(next, 6);
}

// ---------------------------------------------------------------------------
// 空マニフェスト保存/読込
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, EmptyManifest) {
    TemplateManifest m;
    m.version = 1;
    m.root_dir = "empty_dir";

    std::string err;
    ASSERT_TRUE(saveManifestJson(manifest_path_, m, &err)) << err;

    TemplateManifest loaded;
    ASSERT_TRUE(loadManifestJson(manifest_path_, loaded, &err)) << err;
    EXPECT_EQ(loaded.version, 1);
    EXPECT_EQ(loaded.root_dir, "empty_dir");
    EXPECT_EQ(loaded.entries.size(), 0u);
}

// ---------------------------------------------------------------------------
// 存在しないファイル読込失敗
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, LoadNonExistent) {
    TemplateManifest loaded;
    std::string err;
    EXPECT_FALSE(loadManifestJson(temp_dir_ + "/no_such_file.json", loaded, &err));
    EXPECT_FALSE(err.empty());
}

// ---------------------------------------------------------------------------
// 空ファイル読込失敗
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, LoadEmptyFile) {
    // 空ファイルを作成
    {
        std::ofstream ofs(manifest_path_);
    }

    TemplateManifest loaded;
    std::string err;
    EXPECT_FALSE(loadManifestJson(manifest_path_, loaded, &err));
}

// ---------------------------------------------------------------------------
// 書込不可パスへの保存失敗
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, SaveToInvalidPath) {
    TemplateManifest m;
    std::string err;
    // 存在しない深いパスへの書込
    bool ok = saveManifestJson(temp_dir_ + "/nonexistent/deep/path/manifest.json", m, &err);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
}

// ---------------------------------------------------------------------------
// template_idなしエントリはスキップされる
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, EntryWithoutIdSkipped) {
    // template_idがないJSONオブジェクトを手動作成
    std::string json = R"({
  "version": 1,
  "root_dir": "templates",
  "entries": [
    { "name": "no_id_entry", "file": "no_id.png", "w": 10, "h": 10 },
    { "template_id": 42, "name": "valid", "file": "valid.png", "w": 20, "h": 20 }
  ]
})";
    {
        std::ofstream ofs(manifest_path_);
        ofs << json;
    }

    TemplateManifest loaded;
    std::string err;
    ASSERT_TRUE(loadManifestJson(manifest_path_, loaded, &err));
    // template_idなしエントリは findInt 失敗で continue → 1エントリのみ
    ASSERT_EQ(loaded.entries.size(), 1u);
    EXPECT_EQ(loaded.entries[0].template_id, 42);
    EXPECT_EQ(loaded.entries[0].name, "valid");
}

// ---------------------------------------------------------------------------
// 大量エントリの保存/読込
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, ManyEntries) {
    TemplateManifest m;
    m.version = 1;
    m.root_dir = "templates";
    const int N = 200;
    for (int i = 0; i < N; ++i) {
        TemplateEntry e;
        e.template_id = i + 1;
        e.name = "tpl_" + std::to_string(i);
        e.file = "tpl_" + std::to_string(i) + ".png";
        e.w = 32 + i;
        e.h = 32 + i;
        m.entries.push_back(e);
    }

    std::string err;
    ASSERT_TRUE(saveManifestJson(manifest_path_, m, &err)) << err;

    TemplateManifest loaded;
    ASSERT_TRUE(loadManifestJson(manifest_path_, loaded, &err)) << err;
    EXPECT_EQ(loaded.entries.size(), (size_t)N);

    auto idx = indexById(loaded);
    EXPECT_EQ(idx.size(), (size_t)N);
    // 全ID衝突なし
    int next = allocateNextId(loaded, 1);
    EXPECT_EQ(next, N + 1);
}

// ---------------------------------------------------------------------------
// 日本語テンプレート名の保存/読込ラウンドトリップ
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, JapaneseNameRoundTrip) {
    TemplateManifest m;
    m.version = 1;
    m.root_dir = "テンプレート";

    TemplateEntry e;
    e.template_id = 1;
    e.name = "ボタン_OK";
    e.file = "button_ok.png";
    e.w = 64; e.h = 32;
    m.entries.push_back(e);

    std::string err;
    ASSERT_TRUE(saveManifestJson(manifest_path_, m, &err)) << err;

    TemplateManifest loaded;
    ASSERT_TRUE(loadManifestJson(manifest_path_, loaded, &err)) << err;

    EXPECT_EQ(loaded.root_dir, "テンプレート");
    ASSERT_EQ(loaded.entries.size(), 1u);
    EXPECT_EQ(loaded.entries[0].name, "ボタン_OK");
}

// ---------------------------------------------------------------------------
// パス内のバックスラッシュ（Windows）の正常処理
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, WindowsPathBackslash) {
    TemplateManifest m;
    m.version = 1;
    m.root_dir = "C:\\MirageWork\\templates";

    TemplateEntry e;
    e.template_id = 1;
    e.name = "btn";
    e.file = "sub\\dir\\button.png";
    e.w = 32; e.h = 16;
    m.entries.push_back(e);

    std::string err;
    ASSERT_TRUE(saveManifestJson(manifest_path_, m, &err)) << err;

    TemplateManifest loaded;
    ASSERT_TRUE(loadManifestJson(manifest_path_, loaded, &err)) << err;

    EXPECT_EQ(loaded.root_dir, "C:\\MirageWork\\templates");
    ASSERT_EQ(loaded.entries.size(), 1u);
    EXPECT_EQ(loaded.entries[0].file, "sub\\dir\\button.png");
}

// ---------------------------------------------------------------------------
// ダブルクォート含みの説明文（tagsフィールドで検証）
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, DoubleQuoteInTags) {
    TemplateManifest m;
    m.version = 1;
    m.root_dir = "templates";

    TemplateEntry e;
    e.template_id = 1;
    e.name = "btn";
    e.file = "btn.png";
    e.w = 32; e.h = 16;
    e.tags = "label:\"OK\",type:button";
    m.entries.push_back(e);

    std::string err;
    ASSERT_TRUE(saveManifestJson(manifest_path_, m, &err)) << err;

    TemplateManifest loaded;
    ASSERT_TRUE(loadManifestJson(manifest_path_, loaded, &err)) << err;

    ASSERT_EQ(loaded.entries.size(), 1u);
    EXPECT_EQ(loaded.entries[0].tags, "label:\"OK\",type:button");
}

// ---------------------------------------------------------------------------
// root_dir省略時のデフォルト値
// ---------------------------------------------------------------------------
TEST_F(ManifestTest, DefaultRootDir) {
    std::string json = R"({
  "version": 1,
  "entries": []
})";
    {
        std::ofstream ofs(manifest_path_);
        ofs << json;
    }

    TemplateManifest loaded;
    std::string err;
    ASSERT_TRUE(loadManifestJson(manifest_path_, loaded, &err));
    // root_dir未指定時は "templates" がデフォルト
    EXPECT_EQ(loaded.root_dir, "templates");
}
