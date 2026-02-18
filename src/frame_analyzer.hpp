// =============================================================================
// MirageSystem - Frame Analyzer (Tesseract OCR)
// =============================================================================
// EventBus連携フレーム解析。FrameReadyEventからOCRテキスト抽出。
// Usage:
//   mirage::analyzer().init("eng+jpn");
//   mirage::analyzer().startCapture();
//   auto result = mirage::analyzer().analyzeText("device-1");
// =============================================================================
#pragma once

#ifdef MIRAGE_OCR_ENABLED

#include "event_bus.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <map>

namespace mirage {

struct OcrWord {
    std::string text;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // バウンディングボックス
    float confidence = 0.0f;                 // 0-100
};

struct OcrResult {
    std::string device_id;
    std::vector<OcrWord> words;
    int image_width = 0;
    int image_height = 0;
    double elapsed_ms = 0;

    // クエリを含む単語を検索（大文字小文字無視）
    std::vector<OcrWord> findText(const std::string& query) const;

    // 全テキストを連結して返す
    std::string fullText() const;
};

class FrameAnalyzer {
public:
    FrameAnalyzer();
    ~FrameAnalyzer();

    // Tesseract初期化（起動時に1回呼ぶ）
    // langs: "eng", "jpn", "eng+jpn" など
    bool init(const std::string& langs = "eng+jpn");

    // EventBus購読 — デバイスごとに最新フレームをキャッシュ
    void startCapture();
    void stopCapture();

    // OCR API（キャッシュフレーム使用、スレッドセーフ）
    OcrResult analyzeText(const std::string& device_id);
    std::vector<OcrWord> findText(const std::string& device_id, const std::string& query);
    bool hasText(const std::string& device_id, const std::string& query);

    // 最初のマッチの中心座標を取得（タップ用）
    bool getTextCenter(const std::string& device_id, const std::string& query, int& cx, int& cy);

    // 状態
    bool isInitialized() const { return initialized_.load(); }

private:
    struct FrameCache {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        uint64_t frame_id = 0;
    };

    // フレームイベントハンドラ
    void onFrame(const FrameReadyEvent& evt);

    // RGBA生データに対する内部OCR実行
    OcrResult runOcr(const uint8_t* rgba, int w, int h);

    std::atomic<bool> initialized_{false};
    SubscriptionHandle frame_sub_;

    mutable std::mutex frames_mutex_;
    std::map<std::string, FrameCache> frames_;  // device_id -> 最新フレーム

    // Tesseractインスタンス（スレッドセーフでないため ocr_mutex_ で保護）
    std::mutex ocr_mutex_;
    struct TessImpl;
    std::unique_ptr<TessImpl> tess_;
};

// グローバルシングルトン
FrameAnalyzer& analyzer();

} // namespace mirage

#endif // MIRAGE_OCR_ENABLED
