// =============================================================================
// FrameAnalyzer CPUロジック ユニットテスト (Tesseract不要)
// =============================================================================
// MIRAGE_OCR_ENABLED を定義しつつ、Tesseractをリンクせずにロジック層のみをテスト。
// frame_analyzer.cpp はリンクせず、本テスト内でスタブ実装を提供する。
//
// テスト対象:
//   1. FrameAnalyzer構築・破棄（Tesseract未初期化でもクラッシュしない）
//   2. 空フレームへのanalyzeText → 空結果 / device_id保持
//   3. EventBus経由フレームキャッシュの更新・取得（onFrame → analyzeText）
//   4. findText/hasText の文字列マッチングロジック（大文字小文字・部分一致）
//   5. getTextCenter の座標計算（バウンディングボックス中心）
//   6. 複数デバイスのフレーム管理（device_id 別キャッシュ）
// =============================================================================
#include <gtest/gtest.h>

// --- スタブ: mirage_log.hpp ---
// MLOG_* マクロをno-opに置き換えてTesseract/本体依存を排除
#ifndef MIRAGE_LOG_HPP
#define MIRAGE_LOG_HPP
#include <cstdio>
#define MLOG_INFO(tag, fmt, ...)  do {} while(0)
#define MLOG_WARN(tag, fmt, ...)  do {} while(0)
#define MLOG_ERROR(tag, fmt, ...) do {} while(0)
#define MLOG_DEBUG(tag, fmt, ...) do {} while(0)
#endif

// --- MIRAGE_OCR_ENABLED を強制定義（ヘッダを有効化） ---
#ifndef MIRAGE_OCR_ENABLED
#define MIRAGE_OCR_ENABLED 1
#endif

// =============================================================================
// FrameAnalyzerのスタブ実装
// =============================================================================
// frame_analyzer.cpp はTesseract/Leptonicaに依存するためリンクしない。
// 代わりに、ここでロジック層のみを再現したスタブクラスを定義してテストする。
// (構造体はframe_analyzer.hppと同一定義)
// =============================================================================

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstring>

// EventBus (event_bus.hpp) をインクルード — mirage_log.hppスタブ後
#include "event_bus.hpp"

namespace mirage {

// ---- OcrWord / OcrResult ------------------------------------------------

struct OcrWord {
    std::string text;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float confidence = 0.0f;
};

struct OcrResult {
    std::string device_id;
    std::vector<OcrWord> words;
    int image_width  = 0;
    int image_height = 0;
    double elapsed_ms = 0;

    // 大文字小文字無視の部分一致検索
    std::vector<OcrWord> findText(const std::string& query) const {
        std::vector<OcrWord> matches;
        if (query.empty()) return matches;

        std::string lq;
        lq.reserve(query.size());
        for (char c : query)
            lq += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        for (const auto& w : words) {
            std::string lt;
            lt.reserve(w.text.size());
            for (char c : w.text)
                lt += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (lt.find(lq) != std::string::npos)
                matches.push_back(w);
        }
        return matches;
    }

    // 全単語をスペース区切りで連結
    std::string fullText() const {
        std::string result;
        for (size_t i = 0; i < words.size(); ++i) {
            if (i > 0) result += ' ';
            result += words[i].text;
        }
        return result;
    }
};

// ---- FrameAnalyzerStub --------------------------------------------------
// Tesseractを持たず、フレームキャッシュと結果注入機能だけを持つテスト用クラス。

class FrameAnalyzerStub {
public:
    FrameAnalyzerStub() = default;

    ~FrameAnalyzerStub() {
        stopCapture();
    }

    // 初期化: Tesseractなしでも安全に動作する（常にfalse or true選択可）
    bool init(bool pretend_ok = false) {
        initialized_.store(pretend_ok);
        return pretend_ok;
    }

    // EventBus購読 — FrameReadyEvent受信でキャッシュ更新
    void startCapture() {
        frame_sub_ = bus().subscribe<FrameReadyEvent>(
            [this](const FrameReadyEvent& evt) { onFrame(evt); });
    }

    void stopCapture() {
        frame_sub_ = SubscriptionHandle{};
    }

    bool isInitialized() const { return initialized_.load(); }

    // OcrResult取得: 初期化済みで注入済み結果があれば返す
    OcrResult analyzeText(const std::string& device_id) {
        OcrResult result;
        result.device_id = device_id;

        if (!initialized_.load()) return result;

        // 注入済みOCR結果を優先返却
        {
            std::lock_guard<std::mutex> lk(injected_mutex_);
            auto it = injected_results_.find(device_id);
            if (it != injected_results_.end()) return it->second;
        }

        // フレームキャッシュ確認
        std::lock_guard<std::mutex> lk(frames_mutex_);
        auto it = frames_.find(device_id);
        if (it == frames_.end()) return result;

        result.image_width  = it->second.width;
        result.image_height = it->second.height;
        return result;
    }

    std::vector<OcrWord> findText(const std::string& device_id, const std::string& query) {
        return analyzeText(device_id).findText(query);
    }

    bool hasText(const std::string& device_id, const std::string& query) {
        return !findText(device_id, query).empty();
    }

    bool getTextCenter(const std::string& device_id, const std::string& query, int& cx, int& cy) {
        auto matches = findText(device_id, query);
        if (matches.empty()) return false;
        const auto& w = matches[0];
        cx = (w.x1 + w.x2) / 2;
        cy = (w.y1 + w.y2) / 2;
        return true;
    }

    // テスト用: デバイスに対してOCR結果を注入する
    void injectResult(const std::string& device_id, OcrResult result) {
        result.device_id = device_id;
        std::lock_guard<std::mutex> lk(injected_mutex_);
        injected_results_[device_id] = std::move(result);
    }

    // テスト用: キャッシュにデバイスフレームがあるか確認
    bool hasFrame(const std::string& device_id) const {
        std::lock_guard<std::mutex> lk(frames_mutex_);
        return frames_.count(device_id) > 0;
    }

    // テスト用: キャッシュフレームの frame_id 取得
    uint64_t frameId(const std::string& device_id) const {
        std::lock_guard<std::mutex> lk(frames_mutex_);
        auto it = frames_.find(device_id);
        if (it == frames_.end()) return 0;
        return it->second.frame_id;
    }

    // テスト用: キャッシュフレームのサイズ取得
    std::pair<int,int> frameSize(const std::string& device_id) const {
        std::lock_guard<std::mutex> lk(frames_mutex_);
        auto it = frames_.find(device_id);
        if (it == frames_.end()) return {0, 0};
        return {it->second.width, it->second.height};
    }

    int cachedDeviceCount() const {
        std::lock_guard<std::mutex> lk(frames_mutex_);
        return static_cast<int>(frames_.size());
    }

private:
    struct FrameCache {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        uint64_t frame_id = 0;
    };

    void onFrame(const FrameReadyEvent& evt) {
        if (!evt.rgba_data || evt.width <= 0 || evt.height <= 0) return;
        const size_t sz = static_cast<size_t>(evt.width) * evt.height * 4;
        std::lock_guard<std::mutex> lk(frames_mutex_);
        auto& c = frames_[evt.device_id];
        c.rgba.resize(sz);
        std::memcpy(c.rgba.data(), evt.rgba_data, sz);
        c.width    = evt.width;
        c.height   = evt.height;
        c.frame_id = evt.frame_id;
    }

    std::atomic<bool> initialized_{false};
    SubscriptionHandle frame_sub_;

    mutable std::mutex frames_mutex_;
    std::map<std::string, FrameCache> frames_;

    std::mutex injected_mutex_;
    std::map<std::string, OcrResult> injected_results_;
};

} // namespace mirage

using namespace mirage;

// =============================================================================
// OcrResult::findText — 大文字小文字無視・部分一致
// =============================================================================

TEST(OcrResultUnitTest, FindTextCaseInsensitive) {
    OcrResult r;
    r.words = {
        {"Hello",   10, 10, 50, 30, 95.0f},
        {"World",   60, 10, 110, 30, 90.0f},
        {"TESTING", 10, 40, 80, 60, 85.0f},
    };

    // 小文字クエリ → 大文字テキストにマッチ
    auto m = r.findText("hello");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].text, "Hello");

    // 部分一致
    m = r.findText("test");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].text, "TESTING");

    // マッチなし
    EXPECT_TRUE(r.findText("xyz").empty());

    // 空クエリ → 常に空
    EXPECT_TRUE(r.findText("").empty());
}

TEST(OcrResultUnitTest, FindTextMultipleMatches) {
    OcrResult r;
    r.words = {
        {"Login",  10, 10, 50,  30, 95.0f},
        {"login",  60, 10, 110, 30, 90.0f},
        {"LOGIN",  10, 40, 80,  60, 85.0f},
        {"Other",  90, 40, 130, 60, 80.0f},
    };
    // "login" → 大文字小文字3種すべてマッチ
    EXPECT_EQ(r.findText("login").size(), 3u);
}

TEST(OcrResultUnitTest, FindTextPreservesBoundingBox) {
    OcrResult r;
    r.words = {{"Settings", 100, 200, 300, 250, 92.0f}};

    auto m = r.findText("settings");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].x1, 100);
    EXPECT_EQ(m[0].y1, 200);
    EXPECT_EQ(m[0].x2, 300);
    EXPECT_EQ(m[0].y2, 250);
    EXPECT_FLOAT_EQ(m[0].confidence, 92.0f);
}

// =============================================================================
// OcrResult::fullText — スペース区切り連結
// =============================================================================

TEST(OcrResultUnitTest, FullTextConcatenation) {
    OcrResult r;
    r.words = {{"Hello", 0,0,0,0, 90.0f}, {"World", 0,0,0,0, 85.0f}};
    EXPECT_EQ(r.fullText(), "Hello World");
}

TEST(OcrResultUnitTest, FullTextEmpty) {
    OcrResult r;
    EXPECT_EQ(r.fullText(), "");
}

TEST(OcrResultUnitTest, FullTextSingleWord) {
    OcrResult r;
    r.words = {{"Only", 0,0,0,0, 90.0f}};
    EXPECT_EQ(r.fullText(), "Only");
}

// =============================================================================
// FrameAnalyzerStub — 構築・破棄
// =============================================================================

TEST(FrameAnalyzerUnitTest, ConstructDestroy) {
    // Tesseract未初期化でもクラッシュしない
    FrameAnalyzerStub fa;
    EXPECT_FALSE(fa.isInitialized());
    // デストラクタが安全に呼ばれる（何もしない）
}

TEST(FrameAnalyzerUnitTest, InitFalse) {
    FrameAnalyzerStub fa;
    bool ok = fa.init(false); // Tesseractなし想定
    EXPECT_FALSE(ok);
    EXPECT_FALSE(fa.isInitialized());
}

TEST(FrameAnalyzerUnitTest, InitTrue) {
    FrameAnalyzerStub fa;
    bool ok = fa.init(true); // 初期化成功をシミュレート
    EXPECT_TRUE(ok);
    EXPECT_TRUE(fa.isInitialized());
}

// =============================================================================
// startCapture/stopCapture — init前後でもクラッシュしない
// =============================================================================

TEST(FrameAnalyzerUnitTest, StartStopCaptureBeforeInit) {
    FrameAnalyzerStub fa;
    fa.startCapture();
    fa.stopCapture();
    // 二重停止も安全
    fa.stopCapture();
}

TEST(FrameAnalyzerUnitTest, StartStopCaptureAfterInit) {
    FrameAnalyzerStub fa;
    fa.init(true);
    fa.startCapture();
    fa.stopCapture();
}

// =============================================================================
// 空フレームへのanalyzeText — 未初期化時
// =============================================================================

TEST(FrameAnalyzerUnitTest, AnalyzeWithoutInit) {
    FrameAnalyzerStub fa;
    auto result = fa.analyzeText("device-1");
    // 未初期化 → 空結果だがdevice_idは保持
    EXPECT_TRUE(result.words.empty());
    EXPECT_EQ(result.device_id, "device-1");
}

TEST(FrameAnalyzerUnitTest, AnalyzeUnknownDevice) {
    FrameAnalyzerStub fa;
    fa.init(true);
    auto result = fa.analyzeText("unknown-device");
    // フレームキャッシュなし → 空
    EXPECT_TRUE(result.words.empty());
    EXPECT_EQ(result.device_id, "unknown-device");
}

// =============================================================================
// EventBus経由フレームキャッシュ更新
// =============================================================================

TEST(FrameAnalyzerUnitTest, FrameCacheUpdatedViaEventBus) {
    FrameAnalyzerStub fa;
    fa.startCapture();

    // キャプチャ前はキャッシュなし
    EXPECT_FALSE(fa.hasFrame("dev-a"));

    // EventBusにFrameReadyEventを発行
    const int W = 320, H = 240;
    std::vector<uint8_t> pixels(W * H * 4, 128);

    FrameReadyEvent evt;
    evt.device_id = "dev-a";
    evt.rgba_data = pixels.data();
    evt.width     = W;
    evt.height    = H;
    evt.frame_id  = 42;
    bus().publish(evt);

    // キャッシュが更新されている
    EXPECT_TRUE(fa.hasFrame("dev-a"));
    EXPECT_EQ(fa.frameId("dev-a"), 42u);
    auto sz = fa.frameSize("dev-a");
    EXPECT_EQ(sz.first,  W);
    EXPECT_EQ(sz.second, H);

    fa.stopCapture();
}

TEST(FrameAnalyzerUnitTest, FrameCacheOverwriteOnNewFrame) {
    FrameAnalyzerStub fa;
    fa.startCapture();

    const int W = 100, H = 100;
    std::vector<uint8_t> pixels(W * H * 4, 0);

    FrameReadyEvent evt;
    evt.device_id = "dev-b";
    evt.rgba_data = pixels.data();
    evt.width     = W;
    evt.height    = H;
    evt.frame_id  = 1;
    bus().publish(evt);
    EXPECT_EQ(fa.frameId("dev-b"), 1u);

    // 同デバイスの新フレーム
    evt.frame_id = 99;
    bus().publish(evt);
    EXPECT_EQ(fa.frameId("dev-b"), 99u);

    fa.stopCapture();
}

TEST(FrameAnalyzerUnitTest, InvalidFrameIgnored) {
    FrameAnalyzerStub fa;
    fa.startCapture();

    // rgba_data == nullptr → 無視される
    FrameReadyEvent evt;
    evt.device_id = "dev-invalid";
    evt.rgba_data = nullptr;
    evt.width     = 100;
    evt.height    = 100;
    evt.frame_id  = 5;
    bus().publish(evt);

    EXPECT_FALSE(fa.hasFrame("dev-invalid"));

    // width/height == 0 → 無視される
    std::vector<uint8_t> px(4, 0);
    evt.rgba_data = px.data();
    evt.width     = 0;
    evt.height    = 0;
    bus().publish(evt);
    EXPECT_FALSE(fa.hasFrame("dev-invalid"));

    fa.stopCapture();
}

// =============================================================================
// 複数デバイスのフレーム管理（device_id別キャッシュ）
// =============================================================================

TEST(FrameAnalyzerUnitTest, MultiDeviceFrameCache) {
    FrameAnalyzerStub fa;
    fa.startCapture();

    std::vector<uint8_t> px(100 * 100 * 4, 0);
    FrameReadyEvent evt;
    evt.rgba_data = px.data();
    evt.width = 100; evt.height = 100;

    evt.device_id = "device-1"; evt.frame_id = 10; bus().publish(evt);
    evt.device_id = "device-2"; evt.frame_id = 20; bus().publish(evt);
    evt.device_id = "device-3"; evt.frame_id = 30; bus().publish(evt);

    EXPECT_EQ(fa.cachedDeviceCount(), 3);
    EXPECT_EQ(fa.frameId("device-1"), 10u);
    EXPECT_EQ(fa.frameId("device-2"), 20u);
    EXPECT_EQ(fa.frameId("device-3"), 30u);

    fa.stopCapture();
}

// =============================================================================
// stopCapture後はフレームを受け取らない
// =============================================================================

TEST(FrameAnalyzerUnitTest, StopCaptureUnsubscribes) {
    FrameAnalyzerStub fa;
    fa.startCapture();
    fa.stopCapture();

    std::vector<uint8_t> px(100 * 100 * 4, 0);
    FrameReadyEvent evt;
    evt.device_id = "dev-c";
    evt.rgba_data = px.data();
    evt.width     = 100; evt.height = 100; evt.frame_id = 77;
    bus().publish(evt);

    // 購読解除済みなのでキャッシュされない
    EXPECT_FALSE(fa.hasFrame("dev-c"));
}

// =============================================================================
// findText/hasText — OCR結果注入で検証
// =============================================================================

TEST(FrameAnalyzerUnitTest, FindTextWithInjectedResult) {
    FrameAnalyzerStub fa;
    fa.init(true);

    OcrResult ocr;
    ocr.words = {
        {"OK",     10, 10, 50, 30, 95.0f},
        {"Cancel", 60, 10, 120, 30, 90.0f},
    };
    fa.injectResult("dev-x", ocr);

    auto found = fa.findText("dev-x", "ok");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].text, "OK");

    EXPECT_TRUE(fa.hasText("dev-x", "cancel"));
    EXPECT_FALSE(fa.hasText("dev-x", "nonexistent"));
}

TEST(FrameAnalyzerUnitTest, HasTextCaseInsensitive) {
    FrameAnalyzerStub fa;
    fa.init(true);

    OcrResult ocr;
    ocr.words = {{"Settings", 0,0,100,50, 88.0f}};
    fa.injectResult("dev-y", ocr);

    EXPECT_TRUE(fa.hasText("dev-y", "settings"));
    EXPECT_TRUE(fa.hasText("dev-y", "SETTINGS"));
    EXPECT_TRUE(fa.hasText("dev-y", "set"));
    EXPECT_FALSE(fa.hasText("dev-y", "none"));
}

// =============================================================================
// getTextCenter — バウンディングボックス中心座標計算
// =============================================================================

TEST(FrameAnalyzerUnitTest, GetTextCenterCalculation) {
    FrameAnalyzerStub fa;
    fa.init(true);

    OcrResult ocr;
    // x1=100, y1=200, x2=300, y2=250 → center=(200, 225)
    ocr.words = {{"Next", 100, 200, 300, 250, 90.0f}};
    fa.injectResult("dev-z", ocr);

    int cx = -1, cy = -1;
    ASSERT_TRUE(fa.getTextCenter("dev-z", "next", cx, cy));
    EXPECT_EQ(cx, (100 + 300) / 2); // 200
    EXPECT_EQ(cy, (200 + 250) / 2); // 225
}

TEST(FrameAnalyzerUnitTest, GetTextCenterNoMatch) {
    FrameAnalyzerStub fa;
    fa.init(true);

    int cx = -1, cy = -1;
    bool found = fa.getTextCenter("dev-empty", "anything", cx, cy);
    EXPECT_FALSE(found);
    // cx/cy は変更されない
    EXPECT_EQ(cx, -1);
    EXPECT_EQ(cy, -1);
}

TEST(FrameAnalyzerUnitTest, GetTextCenterFirstMatchUsed) {
    FrameAnalyzerStub fa;
    fa.init(true);

    OcrResult ocr;
    // 同じクエリに2件マッチ → 最初の結果を使用
    ocr.words = {
        {"OK", 10, 20, 50, 40, 95.0f},   // center=(30, 30)
        {"OK", 200, 300, 260, 340, 80.0f}, // center=(230, 320)
    };
    fa.injectResult("dev-multi", ocr);

    int cx = -1, cy = -1;
    ASSERT_TRUE(fa.getTextCenter("dev-multi", "ok", cx, cy));
    EXPECT_EQ(cx, (10 + 50) / 2);  // 30
    EXPECT_EQ(cy, (20 + 40) / 2);  // 30
}

// =============================================================================
// analyzeText — image_width/height がキャッシュから引き継がれる
// =============================================================================

TEST(FrameAnalyzerUnitTest, AnalyzeTextReturnsFrameDimensions) {
    FrameAnalyzerStub fa;
    fa.init(true);
    fa.startCapture();

    const int W = 1200, H = 2000;
    std::vector<uint8_t> pixels(W * H * 4, 255);

    FrameReadyEvent evt;
    evt.device_id = "npad-x1";
    evt.rgba_data = pixels.data();
    evt.width = W; evt.height = H; evt.frame_id = 1;
    bus().publish(evt);

    auto result = fa.analyzeText("npad-x1");
    EXPECT_EQ(result.image_width,  W);
    EXPECT_EQ(result.image_height, H);

    fa.stopCapture();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
