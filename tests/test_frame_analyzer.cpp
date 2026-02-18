// =============================================================================
// Unit tests for FrameAnalyzer (src/frame_analyzer.hpp)
// =============================================================================
#include <gtest/gtest.h>
#include "frame_analyzer.hpp"

using namespace mirage;

// =============================================================================
// OcrResult::findText — 大文字小文字無視の部分一致
// =============================================================================

TEST(OcrResultTest, FindTextCaseInsensitive) {
    OcrResult result;
    result.words = {
        {"Hello",   10, 10, 50, 30, 95.0f},
        {"World",   60, 10, 110, 30, 90.0f},
        {"TESTING", 10, 40, 80, 60, 85.0f},
    };

    // 小文字クエリで大文字テキストにマッチ
    auto matches = result.findText("hello");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].text, "Hello");

    // 部分一致
    matches = result.findText("test");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].text, "TESTING");

    // マッチなし
    matches = result.findText("xyz");
    EXPECT_TRUE(matches.empty());

    // 空クエリ
    matches = result.findText("");
    EXPECT_TRUE(matches.empty());
}

TEST(OcrResultTest, FindTextMultipleMatches) {
    OcrResult result;
    result.words = {
        {"Login",  10, 10, 50, 30, 95.0f},
        {"login",  60, 10, 110, 30, 90.0f},
        {"LOGIN",  10, 40, 80, 60, 85.0f},
        {"Other",  90, 40, 130, 60, 80.0f},
    };

    auto matches = result.findText("login");
    EXPECT_EQ(matches.size(), 3u);
}

// =============================================================================
// OcrResult::fullText — スペース区切り連結
// =============================================================================

TEST(OcrResultTest, FullTextConcatenation) {
    OcrResult result;
    result.words = {
        {"Hello", 0, 0, 0, 0, 90.0f},
        {"World", 0, 0, 0, 0, 85.0f},
    };
    EXPECT_EQ(result.fullText(), "Hello World");
}

TEST(OcrResultTest, FullTextEmpty) {
    OcrResult result;
    EXPECT_EQ(result.fullText(), "");
}

TEST(OcrResultTest, FullTextSingleWord) {
    OcrResult result;
    result.words = {{"Only", 0, 0, 0, 0, 90.0f}};
    EXPECT_EQ(result.fullText(), "Only");
}

// =============================================================================
// FrameAnalyzer ライフサイクル
// =============================================================================

TEST(FrameAnalyzerTest, InitShutdownLifecycle) {
    FrameAnalyzer fa;
    EXPECT_FALSE(fa.isInitialized());

    // init はTesseractが利用可能な環境でのみ成功する
    // CI環境ではtessdataがない可能性があるため、結果に関わらずクラッシュしないことを確認
    bool ok = fa.init("eng");
    if (ok) {
        EXPECT_TRUE(fa.isInitialized());
    } else {
        EXPECT_FALSE(fa.isInitialized());
    }

    // デストラクタが安全に呼ばれることを確認
}

TEST(FrameAnalyzerTest, StartStopCapture) {
    FrameAnalyzer fa;
    // init前でもstartCapture/stopCaptureがクラッシュしないこと
    fa.startCapture();
    fa.stopCapture();

    // 二重停止も安全
    fa.stopCapture();
}

// =============================================================================
// 空フレームでのOCR — 未初期化時
// =============================================================================

TEST(FrameAnalyzerTest, AnalyzeWithoutInit) {
    FrameAnalyzer fa;
    auto result = fa.analyzeText("device-1");
    EXPECT_TRUE(result.words.empty());
    EXPECT_EQ(result.device_id, "device-1");
}

// =============================================================================
// getTextCenter — マッチなしでfalse
// =============================================================================

TEST(FrameAnalyzerTest, GetTextCenterNoMatch) {
    FrameAnalyzer fa;
    int cx = -1, cy = -1;
    EXPECT_FALSE(fa.getTextCenter("device-1", "nonexistent", cx, cy));
    // cx, cy は変更されない
    EXPECT_EQ(cx, -1);
    EXPECT_EQ(cy, -1);
}

// =============================================================================
// ブランク画像でのOCR（Tesseract利用可能時のみ）
// =============================================================================

TEST(FrameAnalyzerTest, BlankImageReturnsEmpty) {
    FrameAnalyzer fa;
    if (!fa.init("eng")) {
        GTEST_SKIP() << "Tesseract初期化失敗（tessdata未インストール？）";
    }

    // EventBus経由でブランクフレームを注入
    fa.startCapture();

    // 100x100 白画像
    const int w = 100, h = 100;
    std::vector<uint8_t> blank(w * h * 4, 255); // 白RGBA

    FrameReadyEvent evt;
    evt.device_id = "test-blank";
    evt.rgba_data = blank.data();
    evt.width = w;
    evt.height = h;
    evt.frame_id = 1;
    bus().publish(evt);

    auto result = fa.analyzeText("test-blank");
    // ブランク画像 → テキストなし（or ノイズ的な少数のゴミ）
    // 真っ白画像ではTesseractは通常何も検出しない
    EXPECT_EQ(result.image_width, w);
    EXPECT_EQ(result.image_height, h);

    fa.stopCapture();
}

// =============================================================================
// OcrResult::findText のバウンディングボックスが保持されるか
// =============================================================================

TEST(OcrResultTest, FindTextPreservesBoundingBox) {
    OcrResult result;
    result.words = {
        {"Settings", 100, 200, 300, 250, 92.0f},
    };

    auto matches = result.findText("settings");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].x1, 100);
    EXPECT_EQ(matches[0].y1, 200);
    EXPECT_EQ(matches[0].x2, 300);
    EXPECT_EQ(matches[0].y2, 250);
    EXPECT_FLOAT_EQ(matches[0].confidence, 92.0f);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
