// =============================================================================
// Unit tests for UiFinder XML解析 (src/ai/ui_finder.hpp/cpp)
// GPU不要 — parse_bounds / parse_ui_dump のCPU純粋ロジックテスト
// ADBは使わず、モックexecutorでXMLを直接注入
// =============================================================================
#include <gtest/gtest.h>
#include "ai/ui_finder.hpp"

#include <fstream>
#include <filesystem>
#include <string>

using namespace mirage::ai;

// ===========================================================================
// テスト用ヘルパー: mock ADB executor + XML直接注入
// ===========================================================================

// dump_ui_hierarchy() は下記の流れ:
//   1. executor("shell uiautomator dump ...")
//   2. executor("pull ... <temp_path>")
//   3. temp_pathからファイル読込
// テスト用executorはpull時にtemp_pathに指定XMLを書き込む

class UiFinderParseTest : public ::testing::Test {
protected:
#ifdef _WIN32
    static constexpr const char* TEMP_PATH = "C:\\Windows\\Temp\\mirage_ui.xml";
#else
    static constexpr const char* TEMP_PATH = "/tmp/mirage_ui.xml";
#endif

    UiFinder finder_;

    void SetUp() override {
        // XMLを書き込むモックexecutor
        finder_.set_adb_executor([](const std::string& /*cmd*/) -> std::string {
            return "";  // デフォルトは何もしない
        });
    }

    void TearDown() override {
        std::filesystem::remove(TEMP_PATH);
    }

    // テスト用XMLをtemp_pathに直接書込み、対応するmock executorを設定
    void injectXml(const std::string& xml) {
        finder_.set_adb_executor([this, xml](const std::string& cmd) -> std::string {
            if (cmd.find("pull") != std::string::npos) {
                // pullコマンド時にXMLファイルを書き込む
                std::ofstream ofs(TEMP_PATH);
                ofs << xml;
            }
            return "";
        });
    }
};

// ---------------------------------------------------------------------------
// resource-id検索 — ハイフン含み属性名のパース確認
// P2-1修正: [a-zA-Z0-9_-]+ に変更後は "resource-id" 属性を正常解析できる
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, FindByResourceId_WithHyphen) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="OK" resource-id="com.example:id/button_ok" class="android.widget.Button" clickable="true" enabled="true" bounds="[100,200][300,400]"/>
</hierarchy>)";

    injectXml(xml);

    // resource-id属性がハイフン対応正規表現で正しくパースされる
    auto rid_result = finder_.find_by_resource_id("button_ok");
    ASSERT_TRUE(rid_result.is_ok()) << rid_result.error().message;

    auto& elem = rid_result.value();
    EXPECT_EQ(elem.x, 100);
    EXPECT_EQ(elem.y, 200);
    EXPECT_EQ(elem.width, 200);   // 300 - 100
    EXPECT_EQ(elem.height, 200);  // 400 - 200
    EXPECT_TRUE(elem.clickable);
    EXPECT_TRUE(elem.enabled);
    EXPECT_EQ(elem.resource_id, "com.example:id/button_ok");
}

// ---------------------------------------------------------------------------
// テキスト検索
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, FindByText_Exact) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="Settings" resource-id="" class="android.widget.TextView" clickable="false" enabled="true" bounds="[0,0][100,50]"/>
<node index="1" text="OK" resource-id="" class="android.widget.Button" clickable="true" enabled="true" bounds="[50,100][150,150]"/>
</hierarchy>)";

    injectXml(xml);

    // 完全一致
    auto result = finder_.find_by_text("OK", false);
    ASSERT_TRUE(result.is_ok()) << result.error().message;
    EXPECT_EQ(result.value().text, "OK");
    EXPECT_EQ(result.value().x, 50);
    EXPECT_EQ(result.value().y, 100);
    EXPECT_EQ(result.value().width, 100);
    EXPECT_EQ(result.value().height, 50);
}

// ---------------------------------------------------------------------------
// テキスト部分一致
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, FindByText_Partial) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="Accept all cookies" resource-id="" class="android.widget.Button" clickable="true" enabled="true" bounds="[10,20][200,60]"/>
</hierarchy>)";

    injectXml(xml);

    auto result = finder_.find_by_text("cookies", true);
    ASSERT_TRUE(result.is_ok()) << result.error().message;
    EXPECT_EQ(result.value().text, "Accept all cookies");
}

// ---------------------------------------------------------------------------
// テキスト未検出
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, FindByText_NotFound) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="Hello" resource-id="" class="android.widget.TextView" clickable="false" enabled="true" bounds="[0,0][100,50]"/>
</hierarchy>)";

    injectXml(xml);

    auto result = finder_.find_by_text("Goodbye", false);
    EXPECT_TRUE(result.is_err());
}

// ---------------------------------------------------------------------------
// bounds解析: 正常値 [0,0][100,200]
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, ParseBounds_Normal) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="test" resource-id="" class="android.widget.View" clickable="false" enabled="true" bounds="[0,0][100,200]"/>
</hierarchy>)";

    injectXml(xml);

    auto result = finder_.find_by_text("test", false);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().x, 0);
    EXPECT_EQ(result.value().y, 0);
    EXPECT_EQ(result.value().width, 100);
    EXPECT_EQ(result.value().height, 200);
}

// ---------------------------------------------------------------------------
// bounds解析: 大座標
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, ParseBounds_LargeCoords) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="big" resource-id="" class="android.widget.View" clickable="false" enabled="true" bounds="[1080,1920][2160,3840]"/>
</hierarchy>)";

    injectXml(xml);

    auto result = finder_.find_by_text("big", false);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().x, 1080);
    EXPECT_EQ(result.value().y, 1920);
    EXPECT_EQ(result.value().width, 1080);   // 2160-1080
    EXPECT_EQ(result.value().height, 1920);  // 3840-1920
}

// ---------------------------------------------------------------------------
// 空XML → 要素なし
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, EmptyXml) {
    injectXml("");

    // dump_ui_hierarchyは空XMLを返してエラーになる
    auto result = finder_.find_by_resource_id("anything");
    EXPECT_TRUE(result.is_err());
}

// ---------------------------------------------------------------------------
// 空階層XML (nodeなし)
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, EmptyHierarchy) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
</hierarchy>)";

    injectXml(xml);

    auto result = finder_.find_by_resource_id("anything");
    EXPECT_TRUE(result.is_err());
}

// ---------------------------------------------------------------------------
// bounds属性なしノード → テキスト検索で検証
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, NodeWithoutBounds) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="no_bounds" class="android.widget.View" clickable="false" enabled="true"/>
</hierarchy>)";

    injectXml(xml);

    auto result = finder_.find_by_text("no_bounds", false);
    ASSERT_TRUE(result.is_ok());
    // bounds未パース → デフォルト値 (0,0,0,0)
    EXPECT_EQ(result.value().x, 0);
    EXPECT_EQ(result.value().y, 0);
    EXPECT_EQ(result.value().width, 0);
    EXPECT_EQ(result.value().height, 0);
}

// ---------------------------------------------------------------------------
// class属性の取得 + clickable/enabled
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, ClassAttribute) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="SwitchLabel" class="android.widget.Switch" clickable="true" enabled="false" bounds="[10,10][100,50]"/>
</hierarchy>)";

    injectXml(xml);

    auto result = finder_.find_by_text("SwitchLabel", false);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().class_name, "android.widget.Switch");
    EXPECT_TRUE(result.value().clickable);
    EXPECT_FALSE(result.value().enabled);
}

// ---------------------------------------------------------------------------
// 複数ノード — 部分一致で最初にマッチしたものが返る
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, MultipleNodes_FirstMatch) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="Item First" class="android.widget.TextView" clickable="false" enabled="true" bounds="[0,0][100,50]"/>
<node index="1" text="Item Second" class="android.widget.TextView" clickable="false" enabled="true" bounds="[0,50][100,100]"/>
</hierarchy>)";

    injectXml(xml);

    // テキスト部分一致で最初のnodeが返る
    auto result = finder_.find_by_text("Item", true);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().text, "Item First");
    EXPECT_EQ(result.value().y, 0);
}

// ---------------------------------------------------------------------------
// center_x / center_y の計算
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, CenterCoordinates) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<hierarchy rotation="0">
<node index="0" text="centered" resource-id="" class="android.widget.View" clickable="true" enabled="true" bounds="[100,200][300,400]"/>
</hierarchy>)";

    injectXml(xml);

    auto result = finder_.find_by_text("centered", false);
    ASSERT_TRUE(result.is_ok());
    // x=100, w=200 → center_x = 100 + 200/2 = 200
    EXPECT_EQ(result.value().center_x(), 200);
    // y=200, h=200 → center_y = 200 + 200/2 = 300
    EXPECT_EQ(result.value().center_y(), 300);
}

// ---------------------------------------------------------------------------
// 座標テーブル — add/find
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, CoordinateTable_AddAndFind) {
    CoordinateEntry entry;
    entry.key = "accessibility_switch";
    entry.device_model = "";
    entry.x = 500;
    entry.y = 800;
    entry.description = "アクセシビリティスイッチ";

    finder_.add_coordinate_entry(entry);

    auto result = finder_.find_from_table("accessibility_switch");
    ASSERT_TRUE(result.is_ok()) << result.error().message;
    EXPECT_EQ(result.value().x, 500);
    EXPECT_EQ(result.value().y, 800);
}

// ---------------------------------------------------------------------------
// 座標テーブル — 未登録キー
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, CoordinateTable_NotFound) {
    auto result = finder_.find_from_table("nonexistent");
    EXPECT_TRUE(result.is_err());
}

// ---------------------------------------------------------------------------
// 座標テーブル — デバイスモデルフィルタ
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, CoordinateTable_DeviceModelFilter) {
    CoordinateEntry entry;
    entry.key = "button";
    entry.device_model = "Npad X1";
    entry.x = 100;
    entry.y = 200;

    finder_.add_coordinate_entry(entry);
    finder_.set_device_model("A9");

    // デバイスモデル不一致 → 見つからない
    auto result = finder_.find_from_table("button");
    EXPECT_TRUE(result.is_err());

    // モデルを一致させる
    finder_.set_device_model("Npad X1");
    result = finder_.find_from_table("button");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().x, 100);
}

// ---------------------------------------------------------------------------
// 座標テーブル — 同一キーの上書き
// ---------------------------------------------------------------------------
TEST_F(UiFinderParseTest, CoordinateTable_OverwriteSameKey) {
    CoordinateEntry e1;
    e1.key = "btn";
    e1.device_model = "";
    e1.x = 10;
    e1.y = 20;
    finder_.add_coordinate_entry(e1);

    CoordinateEntry e2;
    e2.key = "btn";
    e2.device_model = "";
    e2.x = 99;
    e2.y = 88;
    finder_.add_coordinate_entry(e2);

    auto result = finder_.find_from_table("btn");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().x, 99);
    EXPECT_EQ(result.value().y, 88);
}
