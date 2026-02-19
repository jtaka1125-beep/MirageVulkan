// =============================================================================
// E2E統合テスト — テンプレート→マッチ→アクション→EventBus 全パイプライン
// =============================================================================
// GPU不要: VulkanTemplateMatcher / VulkanComputeProcessor をモックで置換し、
// AIEngine相当のパイプラインロジックを検証する。
// テスト対象: テンプレート登録→マッチ検出→アクション決定→EventBus発行
// =============================================================================

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <algorithm>
#include <functional>
#include <cmath>

// event_bus.hppがmirage_log.hppをインクルードするため、先にインクルード
#include "result.hpp"
#include "event_bus.hpp"
#include "ai/action_mapper.hpp"
#include "ai/template_store.hpp"
#include "ai/template_capture.hpp"

// =============================================================================
// VulkanTemplateMatcher モック — GPU不要でマッチング結果を注入
// =============================================================================

namespace mirage::vk {

struct VkMatchResult {
    int x = 0;
    int y = 0;
    float score = 0.0f;
    int template_id = 0;
};

} // namespace mirage::vk

// =============================================================================
// MockTemplateMatcher — addTemplate/matchをCPUで模擬
// =============================================================================
// テンプレートGray8データを保持し、matchではNCC簡易計算で検出する。
// 実GPU不要。

class MockTemplateMatcher {
public:
    struct TemplateData {
        std::string name;
        std::string group;
        std::vector<uint8_t> gray;
        int w = 0;
        int h = 0;
    };

    mirage::Result<int> addTemplate(const std::string& name,
                                     const uint8_t* gray_data,
                                     int width, int height,
                                     const std::string& group) {
        if (!gray_data || width <= 0 || height <= 0)
            return mirage::Err<int>("invalid template data");

        int id = next_id_++;
        TemplateData td;
        td.name = name;
        td.group = group;
        td.w = width;
        td.h = height;
        td.gray.assign(gray_data, gray_data + (size_t)width * height);
        templates_[id] = std::move(td);
        return mirage::Ok(id);
    }

    // CPU簡易NCCマッチング（テスト用途：完全一致で高スコア）
    mirage::Result<std::vector<mirage::vk::VkMatchResult>>
    match(const uint8_t* gray_data, int width, int height, float threshold = 0.80f) {
        std::vector<mirage::vk::VkMatchResult> results;
        if (!gray_data || width <= 0 || height <= 0)
            return mirage::Err<std::vector<mirage::vk::VkMatchResult>>("invalid frame");

        for (const auto& [id, tpl] : templates_) {
            if (tpl.w > width || tpl.h > height) continue;

            // スライディングウィンドウでベストマッチ探索
            float best_score = -1.0f;
            int best_x = 0, best_y = 0;

            for (int y = 0; y <= height - tpl.h; y += 2) {
                for (int x = 0; x <= width - tpl.w; x += 2) {
                    float score = computeNCC(gray_data, width, height,
                                              tpl.gray.data(), tpl.w, tpl.h,
                                              x, y);
                    if (score > best_score) {
                        best_score = score;
                        best_x = x;
                        best_y = y;
                    }
                }
            }

            if (best_score >= threshold) {
                mirage::vk::VkMatchResult r;
                r.template_id = id;
                r.x = best_x + tpl.w / 2;  // 中心座標
                r.y = best_y + tpl.h / 2;
                r.score = best_score;
                results.push_back(r);
            }
        }

        return mirage::Ok(std::move(results));
    }

    void clearAll() { templates_.clear(); }
    int getTemplateCount() const { return static_cast<int>(templates_.size()); }

private:
    // 簡易NCC (Normalized Cross-Correlation) — テスト用
    static float computeNCC(const uint8_t* frame, int fw, int /*fh*/,
                             const uint8_t* tpl, int tw, int th,
                             int ox, int oy) {
        double sum_f = 0, sum_t = 0, sum_ff = 0, sum_tt = 0, sum_ft = 0;
        int n = tw * th;

        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                double f = frame[(oy + y) * fw + (ox + x)];
                double t = tpl[y * tw + x];
                sum_f += f;
                sum_t += t;
                sum_ff += f * f;
                sum_tt += t * t;
                sum_ft += f * t;
            }
        }

        double mean_f = sum_f / n;
        double mean_t = sum_t / n;
        double var_f = sum_ff / n - mean_f * mean_f;
        double var_t = sum_tt / n - mean_t * mean_t;

        if (var_f < 1e-6 || var_t < 1e-6) {
            // 分散がほぼゼロ — 両方定数なら完全一致
            return (var_f < 1e-6 && var_t < 1e-6 &&
                    std::abs(mean_f - mean_t) < 1.0) ? 1.0f : 0.0f;
        }

        double cov = sum_ft / n - mean_f * mean_t;
        double ncc = cov / std::sqrt(var_f * var_t);
        return std::max(0.0f, std::min(1.0f, (float)ncc));
    }

    std::unordered_map<int, TemplateData> templates_;
    int next_id_ = 0;
};

// =============================================================================
// AIEngineStub — AIEngine相当のパイプライン（GPU依存を排除）
// =============================================================================
// 実AIEngine::Implのロジックを再現:
//   RGBA→Gray → テンプレートマッチ → アクション決定 → EventBus発行

class AIEngineStub {
public:
    AIEngineStub() : action_mapper_(std::make_unique<mirage::ai::ActionMapper>()) {}

    void setTemplateStore(mirage::ai::TemplateStore* store) { template_store_ = store; }

    mirage::Result<int> addTemplate(const std::string& name,
                                     const uint8_t* gray_data, int w, int h,
                                     const std::string& group = "") {
        auto result = matcher_.addTemplate(name, gray_data, w, h, group);
        if (result.is_err()) return result;

        int id = result.value();
        std::lock_guard<std::mutex> lock(names_mutex_);
        id_to_name_[id] = name;

        // デフォルトアクション: tap:<name>
        action_mapper_->addTemplateAction(name, "tap:" + name);
        return mirage::Ok(id);
    }

    // アクション種別をカスタム登録（error→back等）
    void setTemplateAction(const std::string& name, const std::string& action) {
        action_mapper_->addTemplateAction(name, action);
    }

    // テキストアクション登録（OCRフォールバック用）
    void registerTextAction(const std::string& keyword, const std::string& action) {
        action_mapper_->registerTextAction(keyword, action);
    }

    struct ProcessResult {
        enum class Type { NONE, TAP, BACK, WAIT };
        Type type = Type::NONE;
        int x = 0, y = 0;
        float confidence = 0.0f;
        std::string template_id;
        std::string reason;
    };

    // フレーム処理: Gray8入力 → マッチ → アクション決定 → EventBus発行
    ProcessResult processFrame(const std::string& device_id,
                                const uint8_t* gray_data, int width, int height,
                                float threshold = 0.80f) {
        ProcessResult action;

        auto matchResult = matcher_.match(gray_data, width, height, threshold);
        if (matchResult.is_err()) {
            action.reason = "マッチング失敗";
            return action;
        }
        auto results = std::move(matchResult).value();

        // マッチ結果をOverlay用にキャッシュ
        std::unordered_map<int, std::string> names;
        {
            std::lock_guard<std::mutex> lock(names_mutex_);
            names = id_to_name_;
        }

        // MatchResultEvent をEventBus発行
        if (!results.empty()) {
            mirage::MatchResultEvent evt;
            evt.device_id = device_id;
            evt.frame_id = ++frame_count_;
            for (const auto& r : results) {
                mirage::MatchResultEvent::Match m;
                auto it = names.find(r.template_id);
                m.template_name = (it != names.end()) ? it->second
                    : "tpl_" + std::to_string(r.template_id);
                m.x = r.x;
                m.y = r.y;
                m.score = r.score;
                m.template_id = r.template_id;
                evt.matches.push_back(std::move(m));
            }
            mirage::bus().publish(evt);
        }

        if (results.empty()) {
            // テンプレートマッチ失敗 → OCRフォールバック試行
            auto ocr_action = tryOcrFallback(device_id);
            if (ocr_action.type != ProcessResult::Type::NONE) {
                return ocr_action;
            }
            action.type = ProcessResult::Type::WAIT;
            action.reason = "マッチなし";
            return action;
        }

        // 画面状態分類
        std::vector<mirage::ai::MatchResultLite> lite_matches;
        for (const auto& r : results) {
            mirage::ai::MatchResultLite lm;
            lm.template_id = r.template_id;
            auto it = names.find(r.template_id);
            lm.name = (it != names.end()) ? it->second : "";
            lite_matches.push_back(lm);
        }

        auto state = action_mapper_->classifyState(lite_matches);
        if (state == mirage::ai::ActionMapper::ScreenState::LOADING) {
            action.type = ProcessResult::Type::WAIT;
            action.reason = "ローディング検出";
            return action;
        }

        // ベストマッチ選択
        const auto& best = *std::max_element(results.begin(), results.end(),
            [](const auto& a, const auto& b) { return a.score < b.score; });

        std::string tpl_name;
        {
            auto it = names.find(best.template_id);
            tpl_name = (it != names.end()) ? it->second
                : "tpl_" + std::to_string(best.template_id);
        }

        std::string action_str = action_mapper_->getAction(tpl_name);

        if (action_str.substr(0, 4) == "tap:") {
            action.type = ProcessResult::Type::TAP;
            action.template_id = action_str.substr(4);
            action.x = best.x;
            action.y = best.y;
            action.confidence = best.score;

            // EventBus TapCommandEvent 発行
            mirage::TapCommandEvent tap_evt;
            tap_evt.device_id = device_id;
            tap_evt.x = action.x;
            tap_evt.y = action.y;
            tap_evt.source = mirage::CommandSource::AI;
            mirage::bus().publish(tap_evt);

        } else if (action_str == "back") {
            action.type = ProcessResult::Type::BACK;

            // EventBus KeyCommandEvent(BACK) 発行
            mirage::KeyCommandEvent key_evt;
            key_evt.device_id = device_id;
            key_evt.keycode = 4;  // KEYCODE_BACK
            key_evt.source = mirage::CommandSource::AI;
            mirage::bus().publish(key_evt);
        }

        action.reason = "match=" + tpl_name + " score=" + std::to_string(best.score);

        // デバウンス記録
        {
            std::lock_guard<std::mutex> lock(debounce_mutex_);
            auto now = std::chrono::steady_clock::now();
            debounce_map_[device_id + ":" + tpl_name] = now;
        }

        return action;
    }

    // デバウンスチェック: 同一テンプレート+デバイスが指定期間内に検出されていたらtrue
    bool isDebounced(const std::string& device_id, const std::string& tpl_name,
                      int debounce_ms = 500) {
        std::lock_guard<std::mutex> lock(debounce_mutex_);
        auto key = device_id + ":" + tpl_name;
        auto it = debounce_map_.find(key);
        if (it == debounce_map_.end()) return false;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - it->second).count();
        return elapsed < debounce_ms;
    }

    // デバウンス付きprocessFrame
    ProcessResult processFrameWithDebounce(const std::string& device_id,
                                            const uint8_t* gray_data, int width, int height,
                                            int debounce_ms = 500,
                                            float threshold = 0.80f) {
        // まずマッチング実行
        auto matchResult = matcher_.match(gray_data, width, height, threshold);
        if (matchResult.is_err() || matchResult.value().empty()) {
            ProcessResult r;
            r.type = ProcessResult::Type::WAIT;
            r.reason = "マッチなし";
            return r;
        }

        auto results = std::move(matchResult).value();
        std::unordered_map<int, std::string> names;
        {
            std::lock_guard<std::mutex> lock(names_mutex_);
            names = id_to_name_;
        }

        const auto& best = *std::max_element(results.begin(), results.end(),
            [](const auto& a, const auto& b) { return a.score < b.score; });

        std::string tpl_name;
        {
            auto it = names.find(best.template_id);
            tpl_name = (it != names.end()) ? it->second
                : "tpl_" + std::to_string(best.template_id);
        }

        // デバウンス判定
        if (isDebounced(device_id, tpl_name, debounce_ms)) {
            ProcessResult r;
            r.type = ProcessResult::Type::WAIT;
            r.reason = "debounced: " + tpl_name;
            return r;
        }

        // 通常のprocessFrame
        return processFrame(device_id, gray_data, width, height, threshold);
    }

    void clearTemplates() {
        matcher_.clearAll();
        std::lock_guard<std::mutex> lock(names_mutex_);
        id_to_name_.clear();
    }

    // OCR結果注入（テスト用）
    void injectOcrResult(const std::string& device_id,
                          const std::string& keyword,
                          int cx, int cy) {
        std::lock_guard<std::mutex> lock(ocr_mutex_);
        ocr_results_[device_id].push_back({keyword, cx, cy});
    }

    void clearOcrResults() {
        std::lock_guard<std::mutex> lock(ocr_mutex_);
        ocr_results_.clear();
    }

private:
    ProcessResult tryOcrFallback(const std::string& device_id) {
        ProcessResult action;
        action.type = ProcessResult::Type::NONE;

        auto keywords = action_mapper_->getTextKeywords();
        if (keywords.empty()) return action;

        std::lock_guard<std::mutex> lock(ocr_mutex_);
        auto dev_it = ocr_results_.find(device_id);
        if (dev_it == ocr_results_.end()) return action;

        for (const auto& keyword : keywords) {
            for (const auto& ocr : dev_it->second) {
                if (ocr.keyword == keyword) {
                    std::string action_str = action_mapper_->getTextAction(keyword);

                    if (action_str.substr(0, 4) == "tap:") {
                        action.type = ProcessResult::Type::TAP;
                        action.template_id = action_str.substr(4);
                        action.x = ocr.cx;
                        action.y = ocr.cy;
                    } else if (action_str == "back") {
                        action.type = ProcessResult::Type::BACK;
                    }
                    action.reason = "OCR match=\"" + keyword + "\"";

                    // EventBusコマンド発行
                    if (action.type == ProcessResult::Type::TAP) {
                        mirage::TapCommandEvent evt;
                        evt.device_id = device_id;
                        evt.x = action.x;
                        evt.y = action.y;
                        evt.source = mirage::CommandSource::AI;
                        mirage::bus().publish(evt);
                    } else if (action.type == ProcessResult::Type::BACK) {
                        mirage::KeyCommandEvent evt;
                        evt.device_id = device_id;
                        evt.keycode = 4;
                        evt.source = mirage::CommandSource::AI;
                        mirage::bus().publish(evt);
                    }

                    return action;
                }
            }
        }

        return action;
    }

    MockTemplateMatcher matcher_;
    std::unique_ptr<mirage::ai::ActionMapper> action_mapper_;
    mirage::ai::TemplateStore* template_store_ = nullptr;

    std::mutex names_mutex_;
    std::unordered_map<int, std::string> id_to_name_;

    std::mutex debounce_mutex_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> debounce_map_;

    uint64_t frame_count_ = 0;

    // OCRモック結果
    struct OcrEntry {
        std::string keyword;
        int cx = 0, cy = 0;
    };
    std::mutex ocr_mutex_;
    std::unordered_map<std::string, std::vector<OcrEntry>> ocr_results_;
};

// =============================================================================
// テストヘルパー — Gray8フレーム生成
// =============================================================================

// 白背景（255）のGray8フレーム
static std::vector<uint8_t> makeGray8Frame(int w, int h, uint8_t fill = 255) {
    return std::vector<uint8_t>((size_t)w * h, fill);
}

// 白背景に黒四角パターンを描画
static std::vector<uint8_t> makeGray8FrameWithBlackRect(
    int fw, int fh,
    int rx, int ry, int rw, int rh,
    uint8_t bg = 255, uint8_t fg = 0)
{
    auto frame = makeGray8Frame(fw, fh, bg);
    for (int y = ry; y < ry + rh && y < fh; ++y) {
        for (int x = rx; x < rx + rw && x < fw; ++x) {
            frame[y * fw + x] = fg;
        }
    }
    return frame;
}

// 黒四角テンプレート（Gray8）
static std::vector<uint8_t> makeBlackRectTemplate(int w, int h, uint8_t val = 0) {
    return std::vector<uint8_t>((size_t)w * h, val);
}

// =============================================================================
// EventBus受信ヘルパー
// =============================================================================

class EventCollector {
public:
    void startListening() {
        tap_sub_ = mirage::bus().subscribe<mirage::TapCommandEvent>(
            [this](const mirage::TapCommandEvent& evt) {
                std::lock_guard<std::mutex> lock(mutex_);
                tap_events_.push_back(evt);
            });
        key_sub_ = mirage::bus().subscribe<mirage::KeyCommandEvent>(
            [this](const mirage::KeyCommandEvent& evt) {
                std::lock_guard<std::mutex> lock(mutex_);
                key_events_.push_back(evt);
            });
        match_sub_ = mirage::bus().subscribe<mirage::MatchResultEvent>(
            [this](const mirage::MatchResultEvent& evt) {
                std::lock_guard<std::mutex> lock(mutex_);
                match_events_.push_back(evt);
            });
    }

    void stopListening() {
        tap_sub_ = mirage::SubscriptionHandle();
        key_sub_ = mirage::SubscriptionHandle();
        match_sub_ = mirage::SubscriptionHandle();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        tap_events_.clear();
        key_events_.clear();
        match_events_.clear();
    }

    std::vector<mirage::TapCommandEvent> tapEvents() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tap_events_;
    }

    std::vector<mirage::KeyCommandEvent> keyEvents() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return key_events_;
    }

    std::vector<mirage::MatchResultEvent> matchEvents() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return match_events_;
    }

    int tapCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(tap_events_.size());
    }

    int keyCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(key_events_.size());
    }

    int matchCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(match_events_.size());
    }

private:
    mutable std::mutex mutex_;
    std::vector<mirage::TapCommandEvent> tap_events_;
    std::vector<mirage::KeyCommandEvent> key_events_;
    std::vector<mirage::MatchResultEvent> match_events_;

    mirage::SubscriptionHandle tap_sub_;
    mirage::SubscriptionHandle key_sub_;
    mirage::SubscriptionHandle match_sub_;
};

// =============================================================================
// Test 1: テンプレート登録→マッチ→アクション発行の全フロー
// =============================================================================

TEST(AIE2ETest, TemplateRegisterMatchAction) {
    AIEngineStub engine;
    EventCollector collector;
    collector.startListening();

    // 100x100白背景に(30,30)位置に20x20黒四角パターン
    const int FW = 100, FH = 100;
    const int RX = 30, RY = 30, RW = 20, RH = 20;

    auto frame = makeGray8FrameWithBlackRect(FW, FH, RX, RY, RW, RH);

    // テンプレート登録（黒四角パターン切り出し）
    auto tpl = makeBlackRectTemplate(RW, RH);
    auto addResult = engine.addTemplate("black_square", tpl.data(), RW, RH);
    ASSERT_TRUE(addResult.is_ok()) << addResult.error().message;

    // processFrame呼び出し → マッチ検出 → アクション決定
    auto action = engine.processFrame("dev1", frame.data(), FW, FH);

    // TAP アクションが発行されること
    EXPECT_EQ(action.type, AIEngineStub::ProcessResult::Type::TAP);
    EXPECT_GT(action.confidence, 0.80f);
    EXPECT_EQ(action.template_id, "black_square");

    // マッチ座標がテンプレート配置位置の中心付近であること
    EXPECT_NEAR(action.x, RX + RW / 2, 5);
    EXPECT_NEAR(action.y, RY + RH / 2, 5);

    // EventBusでTapCommandEvent受信確認
    auto taps = collector.tapEvents();
    ASSERT_GE(taps.size(), 1u);
    EXPECT_EQ(taps[0].device_id, "dev1");
    EXPECT_NEAR(taps[0].x, RX + RW / 2, 5);
    EXPECT_NEAR(taps[0].y, RY + RH / 2, 5);
    EXPECT_EQ(taps[0].source, mirage::CommandSource::AI);

    // MatchResultEvent も受信していること
    auto matches = collector.matchEvents();
    ASSERT_GE(matches.size(), 1u);
    EXPECT_EQ(matches[0].device_id, "dev1");
    EXPECT_FALSE(matches[0].matches.empty());
    EXPECT_EQ(matches[0].matches[0].template_name, "black_square");

    collector.stopListening();
}

// =============================================================================
// Test 2: OCRフォールバックフロー
// =============================================================================

TEST(AIE2ETest, OcrFallbackFlow) {
    AIEngineStub engine;
    EventCollector collector;
    collector.startListening();

    // テンプレート未登録 → マッチ失敗になる
    // OCRテキストアクション登録
    engine.registerTextAction("OK", "tap:ok_button");

    // OCR結果注入（デバイス"dev1"で"OK"テキストが(150,250)にある）
    engine.injectOcrResult("dev1", "OK", 150, 250);

    // マッチ対象テンプレートなしのフレーム
    auto frame = makeGray8Frame(200, 300, 128);

    auto action = engine.processFrame("dev1", frame.data(), 200, 300);

    // OCRフォールバックでTAPアクション
    EXPECT_EQ(action.type, AIEngineStub::ProcessResult::Type::TAP);
    EXPECT_EQ(action.template_id, "ok_button");
    EXPECT_EQ(action.x, 150);
    EXPECT_EQ(action.y, 250);
    EXPECT_NE(action.reason.find("OCR"), std::string::npos);

    // EventBusでTapCommandEvent受信
    auto taps = collector.tapEvents();
    ASSERT_GE(taps.size(), 1u);
    EXPECT_EQ(taps[0].device_id, "dev1");
    EXPECT_EQ(taps[0].x, 150);
    EXPECT_EQ(taps[0].y, 250);
    EXPECT_EQ(taps[0].source, mirage::CommandSource::AI);

    collector.stopListening();
}

// =============================================================================
// Test 3: エラーポップアップ検出→自動回復フロー
// =============================================================================

TEST(AIE2ETest, ErrorPopupAutoRecovery) {
    AIEngineStub engine;
    EventCollector collector;
    collector.startListening();

    // "error_popup" テンプレート登録 → アクションを "back" に設定
    const int TW = 16, TH = 16;
    auto tpl = makeBlackRectTemplate(TW, TH, 30);  // 暗めのパターン
    auto addResult = engine.addTemplate("error_popup_close", tpl.data(), TW, TH);
    ASSERT_TRUE(addResult.is_ok()) << addResult.error().message;

    // アクションを "back" にオーバーライド
    engine.setTemplateAction("error_popup_close", "back");

    // フレームにテンプレートと同じパターンを配置
    const int FW = 80, FH = 80;
    auto frame = makeGray8FrameWithBlackRect(FW, FH, 20, 20, TW, TH, 255, 30);

    auto action = engine.processFrame("dev1", frame.data(), FW, FH);

    // BACK キー発行
    EXPECT_EQ(action.type, AIEngineStub::ProcessResult::Type::BACK);

    // EventBusでKeyCommandEvent受信確認
    auto keys = collector.keyEvents();
    ASSERT_GE(keys.size(), 1u);
    EXPECT_EQ(keys[0].device_id, "dev1");
    EXPECT_EQ(keys[0].keycode, 4);  // KEYCODE_BACK
    EXPECT_EQ(keys[0].source, mirage::CommandSource::AI);

    collector.stopListening();
}

// =============================================================================
// Test 4: マルチデバイス同時処理
// =============================================================================

TEST(AIE2ETest, MultiDeviceIndependent) {
    AIEngineStub engine;
    EventCollector collector;
    collector.startListening();

    // デバイス1用テンプレート（10x10黒四角、位置(10,10)）
    const int TW = 10, TH = 10;
    auto tpl = makeBlackRectTemplate(TW, TH);
    auto id1 = engine.addTemplate("btn_dev1", tpl.data(), TW, TH);
    ASSERT_TRUE(id1.is_ok());

    // デバイス1のフレーム — テンプレート配置あり
    const int FW = 60, FH = 60;
    auto frame1 = makeGray8FrameWithBlackRect(FW, FH, 10, 10, TW, TH);

    // デバイス2のフレーム — テンプレート配置なし（全白）
    auto frame2 = makeGray8Frame(FW, FH, 255);

    // dev1: マッチあり → TAPアクション
    auto action1 = engine.processFrame("dev1", frame1.data(), FW, FH);
    EXPECT_EQ(action1.type, AIEngineStub::ProcessResult::Type::TAP);
    EXPECT_EQ(action1.template_id, "btn_dev1");

    // dev2: マッチなし → WAITアクション
    auto action2 = engine.processFrame("dev2", frame2.data(), FW, FH);
    EXPECT_EQ(action2.type, AIEngineStub::ProcessResult::Type::WAIT);

    // EventBus: dev1のTapCommandEventのみ受信
    auto taps = collector.tapEvents();
    ASSERT_EQ(taps.size(), 1u);
    EXPECT_EQ(taps[0].device_id, "dev1");

    // dev1のMatchResultEventのみ受信
    auto matches = collector.matchEvents();
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].device_id, "dev1");

    collector.stopListening();
}

// =============================================================================
// Test 4b: マルチデバイス — 各デバイスが異なるテンプレートにマッチ
// =============================================================================

TEST(AIE2ETest, MultiDeviceDifferentMatches) {
    AIEngineStub engine;
    EventCollector collector;
    collector.startListening();

    const int FW = 80, FH = 80;

    // テンプレートA: 値50の12x12パターン
    auto tplA = makeBlackRectTemplate(12, 12, 50);
    auto idA = engine.addTemplate("btn_a", tplA.data(), 12, 12);
    ASSERT_TRUE(idA.is_ok());

    // テンプレートB: 値100の12x12パターン
    auto tplB = makeBlackRectTemplate(12, 12, 100);
    auto idB = engine.addTemplate("btn_b", tplB.data(), 12, 12);
    ASSERT_TRUE(idB.is_ok());

    // dev1フレーム: テンプレートAパターン配置
    auto frame1 = makeGray8FrameWithBlackRect(FW, FH, 20, 20, 12, 12, 255, 50);

    // dev2フレーム: テンプレートBパターン配置
    auto frame2 = makeGray8FrameWithBlackRect(FW, FH, 40, 40, 12, 12, 255, 100);

    auto action1 = engine.processFrame("dev1", frame1.data(), FW, FH);
    auto action2 = engine.processFrame("dev2", frame2.data(), FW, FH);

    // 両方TAPアクション
    EXPECT_EQ(action1.type, AIEngineStub::ProcessResult::Type::TAP);
    EXPECT_EQ(action2.type, AIEngineStub::ProcessResult::Type::TAP);

    // 座標が独立していること
    EXPECT_NEAR(action1.x, 20 + 6, 5);
    EXPECT_NEAR(action1.y, 20 + 6, 5);
    EXPECT_NEAR(action2.x, 40 + 6, 5);
    EXPECT_NEAR(action2.y, 40 + 6, 5);

    // EventBus: 各デバイスのTapが発行されていること
    auto taps = collector.tapEvents();
    ASSERT_EQ(taps.size(), 2u);

    // device_idで検索
    bool found_dev1 = false, found_dev2 = false;
    for (const auto& t : taps) {
        if (t.device_id == "dev1") found_dev1 = true;
        if (t.device_id == "dev2") found_dev2 = true;
    }
    EXPECT_TRUE(found_dev1);
    EXPECT_TRUE(found_dev2);

    collector.stopListening();
}

// =============================================================================
// Test 5: デバウンス動作確認
// =============================================================================

TEST(AIE2ETest, DebounceSuppress) {
    AIEngineStub engine;
    EventCollector collector;
    collector.startListening();

    const int FW = 60, FH = 60;
    const int TW = 10, TH = 10;

    auto tpl = makeBlackRectTemplate(TW, TH);
    auto addResult = engine.addTemplate("repeat_btn", tpl.data(), TW, TH);
    ASSERT_TRUE(addResult.is_ok());

    auto frame = makeGray8FrameWithBlackRect(FW, FH, 20, 20, TW, TH);

    // 1回目: 正常にアクション発行
    auto action1 = engine.processFrameWithDebounce("dev1", frame.data(), FW, FH, 1000);
    EXPECT_EQ(action1.type, AIEngineStub::ProcessResult::Type::TAP);

    // 2回目: デバウンス期間内なのでWAIT（アクション抑制）
    auto action2 = engine.processFrameWithDebounce("dev1", frame.data(), FW, FH, 1000);
    EXPECT_EQ(action2.type, AIEngineStub::ProcessResult::Type::WAIT);
    EXPECT_NE(action2.reason.find("debounced"), std::string::npos);

    // 3回目: まだデバウンス期間内
    auto action3 = engine.processFrameWithDebounce("dev1", frame.data(), FW, FH, 1000);
    EXPECT_EQ(action3.type, AIEngineStub::ProcessResult::Type::WAIT);

    // TapCommandEventは1回のみ発行されていること
    auto taps = collector.tapEvents();
    EXPECT_EQ(taps.size(), 1u);

    collector.stopListening();
}

// =============================================================================
// Test 5b: デバウンス — 期間経過後は再発行
// =============================================================================

TEST(AIE2ETest, DebounceExpiry) {
    AIEngineStub engine;
    EventCollector collector;
    collector.startListening();

    const int FW = 60, FH = 60;
    const int TW = 10, TH = 10;

    auto tpl = makeBlackRectTemplate(TW, TH);
    auto addResult = engine.addTemplate("btn", tpl.data(), TW, TH);
    ASSERT_TRUE(addResult.is_ok());

    auto frame = makeGray8FrameWithBlackRect(FW, FH, 20, 20, TW, TH);

    // 1回目: アクション発行
    auto action1 = engine.processFrameWithDebounce("dev1", frame.data(), FW, FH, 50);
    EXPECT_EQ(action1.type, AIEngineStub::ProcessResult::Type::TAP);

    // デバウンス期間経過を待つ
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // 2回目: デバウンス期間経過後 → 再発行
    auto action2 = engine.processFrameWithDebounce("dev1", frame.data(), FW, FH, 50);
    EXPECT_EQ(action2.type, AIEngineStub::ProcessResult::Type::TAP);

    // TapCommandEventは2回発行されていること
    auto taps = collector.tapEvents();
    EXPECT_EQ(taps.size(), 2u);

    collector.stopListening();
}

// =============================================================================
// Test 6: マッチなし時のWAITアクション
// =============================================================================

TEST(AIE2ETest, NoMatchReturnsWait) {
    AIEngineStub engine;
    EventCollector collector;
    collector.startListening();

    // テンプレート登録なし
    auto frame = makeGray8Frame(100, 100, 128);

    auto action = engine.processFrame("dev1", frame.data(), 100, 100);

    EXPECT_EQ(action.type, AIEngineStub::ProcessResult::Type::WAIT);
    EXPECT_NE(action.reason.find("マッチなし"), std::string::npos);

    // EventBus: コマンドイベント未発行
    EXPECT_EQ(collector.tapCount(), 0);
    EXPECT_EQ(collector.keyCount(), 0);
    EXPECT_EQ(collector.matchCount(), 0);

    collector.stopListening();
}

// =============================================================================
// Test 7: TemplateStore連携 — registerGray8 → MockMatcher登録
// =============================================================================

TEST(AIE2ETest, TemplateStoreIntegration) {
    mirage::ai::TemplateStore store;
    AIEngineStub engine;
    engine.setTemplateStore(&store);

    // TemplateStoreにGray8データを登録（黒=0、最大コントラスト）
    const int TW = 10, TH = 10;
    auto tpl_data = makeBlackRectTemplate(TW, TH, 0);
    auto storeResult = store.registerGray8(1, tpl_data.data(), TW, TH, "test.png");
    ASSERT_TRUE(storeResult.is_ok()) << storeResult.error().message;

    // Store内データを取得してエンジンに登録
    auto* handle = store.get(1);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(handle->w, TW);
    EXPECT_EQ(handle->h, TH);

    auto addResult = engine.addTemplate("from_store",
                                         handle->gray_data.data(),
                                         handle->w, handle->h);
    ASSERT_TRUE(addResult.is_ok());

    // フレームにパターン配置 → マッチ検出（偶数座標配置）
    auto frame = makeGray8FrameWithBlackRect(60, 60, 14, 14, TW, TH, 255, 0);
    auto action = engine.processFrame("dev1", frame.data(), 60, 60);

    EXPECT_EQ(action.type, AIEngineStub::ProcessResult::Type::TAP);
    EXPECT_EQ(action.template_id, "from_store");
}

// =============================================================================
// Test 8: Gray8フレーム生成（template_capture連携）
// =============================================================================

TEST(AIE2ETest, CaptureToGray8AndMatch) {
    // RGBAフレームからROI切り出し → Gray8 → マッチング
    const int FW = 50, FH = 50;
    // 白背景RGBAフレームに(10,10)から15x15の黒四角
    std::vector<uint8_t> rgba(FW * FH * 4);
    for (int i = 0; i < FW * FH; ++i) {
        rgba[i * 4 + 0] = 255;  // R
        rgba[i * 4 + 1] = 255;  // G
        rgba[i * 4 + 2] = 255;  // B
        rgba[i * 4 + 3] = 255;  // A
    }
    // 黒四角描画
    for (int y = 10; y < 25; ++y) {
        for (int x = 10; x < 25; ++x) {
            int idx = (y * FW + x) * 4;
            rgba[idx + 0] = 0;
            rgba[idx + 1] = 0;
            rgba[idx + 2] = 0;
        }
    }

    // ROI切り出し → Gray8
    mirage::ai::RoiRect roi{10, 10, 15, 15};
    auto captureResult = mirage::ai::captureTemplateGray8FromBuffer(
        rgba.data(), FW, FH, roi);
    ASSERT_TRUE(captureResult.is_ok()) << captureResult.error().message;

    auto& gray_tpl = captureResult.value();
    EXPECT_EQ(gray_tpl.w, 15);
    EXPECT_EQ(gray_tpl.h, 15);

    // テンプレートとして登録
    AIEngineStub engine;
    auto addResult = engine.addTemplate("captured_roi",
                                         gray_tpl.pix.data(),
                                         gray_tpl.w, gray_tpl.h);
    ASSERT_TRUE(addResult.is_ok());

    // フルフレームをGray8に変換してマッチング
    std::vector<uint8_t> frame_gray(FW * FH);
    for (int i = 0; i < FW * FH; ++i) {
        int r = rgba[i * 4 + 0];
        int g = rgba[i * 4 + 1];
        int b = rgba[i * 4 + 2];
        frame_gray[i] = (uint8_t)((77 * r + 150 * g + 29 * b + 128) >> 8);
    }

    auto action = engine.processFrame("dev1", frame_gray.data(), FW, FH);
    EXPECT_EQ(action.type, AIEngineStub::ProcessResult::Type::TAP);
    EXPECT_EQ(action.template_id, "captured_roi");
}

// =============================================================================
// Test 9: ローディング画面検出 → WAIT
// =============================================================================

TEST(AIE2ETest, LoadingScreenDetection) {
    AIEngineStub engine;

    const int TW = 10, TH = 10;
    auto tpl = makeBlackRectTemplate(TW, TH, 0);
    auto addResult = engine.addTemplate("loading_spinner", tpl.data(), TW, TH);
    ASSERT_TRUE(addResult.is_ok());

    // classifyState は MatchResultLite の name に "loading" が含まれるとLOADING判定。
    // 偶数座標に配置して確実にマッチさせる
    auto frame = makeGray8FrameWithBlackRect(60, 60, 14, 14, TW, TH, 255, 0);
    auto action = engine.processFrame("dev1", frame.data(), 60, 60);

    // "loading_spinner" がマッチ → classifyState=LOADING → WAIT
    EXPECT_EQ(action.type, AIEngineStub::ProcessResult::Type::WAIT);
    EXPECT_NE(action.reason.find("ローディング"), std::string::npos);
}

// =============================================================================
// Test 10: EventBusイベントの正確な内容検証
// =============================================================================

TEST(AIE2ETest, EventBusPayloadValidation) {
    AIEngineStub engine;
    EventCollector collector;
    collector.startListening();

    const int FW = 80, FH = 80;
    const int TW = 14, TH = 14;

    auto tpl = makeBlackRectTemplate(TW, TH, 0);
    auto addResult = engine.addTemplate("precise_btn", tpl.data(), TW, TH);
    ASSERT_TRUE(addResult.is_ok());

    auto frame = makeGray8FrameWithBlackRect(FW, FH, 30, 24, TW, TH, 255, 0);
    auto action = engine.processFrame("device_abc", frame.data(), FW, FH);

    ASSERT_EQ(action.type, AIEngineStub::ProcessResult::Type::TAP);

    // MatchResultEvent 内容検証
    auto matches = collector.matchEvents();
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].device_id, "device_abc");
    EXPECT_GT(matches[0].frame_id, 0u);
    ASSERT_EQ(matches[0].matches.size(), 1u);
    EXPECT_EQ(matches[0].matches[0].template_name, "precise_btn");
    EXPECT_GT(matches[0].matches[0].score, 0.80f);

    // TapCommandEvent 内容検証
    auto taps = collector.tapEvents();
    ASSERT_EQ(taps.size(), 1u);
    EXPECT_EQ(taps[0].device_id, "device_abc");
    EXPECT_EQ(taps[0].source, mirage::CommandSource::AI);
    // 座標は MatchResult と一致
    EXPECT_EQ(taps[0].x, matches[0].matches[0].x);
    EXPECT_EQ(taps[0].y, matches[0].matches[0].y);

    collector.stopListening();
}
