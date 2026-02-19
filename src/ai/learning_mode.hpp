// =============================================================================
// MirageSystem - Learning Mode
// =============================================================================
// フレームROI切出し → Gray8テンプレート保存 → template_manifest登録。
// EventBus FrameReadyEvent を購読してフレームキャッシュを保持し、
// LearningStartEvent をトリガにテンプレートを生成する。
//
// Usage:
//   mirage::ai::LearningMode lm;
//   lm.start();                        // FrameReadyEvent 購読開始
//   bus().publish(LearningStartEvent{...}); // テンプレートキャプチャ要求
//   // -> LearningCaptureEvent が結果を publish
//   lm.stop();
// =============================================================================
#pragma once

#include "../event_bus.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

#include "template_manifest.hpp"
#include "template_capture.hpp"

namespace mirage::ai {

// テンプレート学習設定
struct LearnConfig {
    std::string templates_dir  = "templates";
    std::string manifest_path  = "templates/manifest.json";
    bool add_timestamp = true;
};

// テンプレート学習結果
struct LearnResult {
    bool ok = false;
    std::string error;
    int template_id = -1;
    int w = 0, h = 0;
    std::string saved_file_rel;
};

// =============================================================================
// LearningMode - EventBus駆動のテンプレート学習
// =============================================================================
class LearningMode {
public:
    explicit LearningMode(const LearnConfig& cfg = {});
    ~LearningMode();

    // FrameReadyEvent 購読を開始/停止
    void start();
    void stop();
    bool is_running() const;

    // 設定変更
    void set_config(const LearnConfig& cfg);
    LearnConfig config() const;

    // 手動呼出し（EventBus経由ではなく直接実行したい場合）
    LearnResult learnFromCachedFrame(
        const std::string& device_id,
        const std::string& name_stem,
        int roi_x, int roi_y, int roi_w, int roi_h);

private:
    // RGBA → Gray8 ROI切出し
    static Gray8 extractRoiGray8(
        const uint8_t* rgba_data, int frame_w, int frame_h,
        int roi_x, int roi_y, int roi_w, int roi_h);

    // タイムスタンプ文字列生成
    static std::string nowStamp();

    // LearningStartEvent ハンドラ
    void onLearningStart(const LearningStartEvent& e);

    // フレームキャッシュ（最新フレームを保持）
    struct FrameCache {
        std::string device_id;
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        uint64_t frame_id = 0;
    };

    mutable std::mutex mutex_;
    LearnConfig config_;
    std::unordered_map<std::string, FrameCache> frame_cache_; // device_id -> cache
    SubscriptionHandle frame_sub_;
    SubscriptionHandle learn_sub_;
    bool running_ = false;
};

} // namespace mirage::ai
