// =============================================================================
// MirageSystem - Continuous Learning v2
// =============================================================================
// Layer1マッチ結果を自動監視し、低信頼度・未検出時にテンプレートを自動学習。
// - VkMatchResult のスコアが閾値以下 or ヒットなしで自動キャプチャ判断
// - 既存テンプレートとの dedup (IoU/center距離) で重複排除
// - template_hot_reload 経由でリアルタイム反映
// - 過学習防止: 同一フレーム位置からの連続登録をクールダウンで制御
//
// Usage:
//   mirage::ai::ContinuousLearningV2 clv2(cfg);
//   clv2.start();
//   // processFrame() 内から onLayer1Result() を呼ぶ
//   clv2.stop();
// =============================================================================
#pragma once

#include "../event_bus.hpp"
#include "ui_element.hpp"
#include "template_manifest.hpp"
#include "template_capture.hpp"

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <unordered_map>

namespace mirage::vk {
class VulkanTemplateMatcher;
}

namespace mirage::ai {

class TemplateStore;

// =============================================================================
// 設定
// =============================================================================
struct CLv2Config {
    float  confidence_threshold = 0.65f;  // これ以下で学習候補
    int    min_roi_size         = 32;     // 最小ROIサイズ(px)
    int    cooldown_frames      = 60;     // 同一領域の再学習抑制
    int    max_templates_total  = 200;    // テンプレート上限
    bool   enabled              = false;  // デフォルトOFF（config.jsonで制御）
    std::string templates_dir   = "templates/auto";
    std::string manifest_path   = "templates/manifest.json";
};

// =============================================================================
// ContinuousLearningV2
// =============================================================================
class ContinuousLearningV2 {
public:
    explicit ContinuousLearningV2(const CLv2Config& cfg = {});
    ~ContinuousLearningV2();

    void start();
    void stop();
    bool is_running() const;
    void set_config(const CLv2Config& cfg);
    CLv2Config get_config() const;

    // 統計情報
    struct Stats {
        int total_candidates       = 0;
        int total_learned          = 0;
        int total_skipped_dedup    = 0;
        int total_skipped_cooldown = 0;
    };
    Stats stats() const;

    // Layer1結果を受け取り学習判断
    // vk_results: テンプレートマッチ結果のスコア配列
    // best_score: 最高スコア（ヒットなしなら 0.0f）
    // rgba_data: フレームRGBAバッファ（学習時のROI切出し用）
    void onLayer1Result(float best_score,
                        const std::string& device_id,
                        int frame_w, int frame_h,
                        const uint8_t* rgba_data);

private:
    // 既存テンプレートとのdedup確認
    bool isDuplicate(const Rect& roi, const std::string& device_id) const;

    // クールダウン確認
    bool isInCooldown(const Rect& roi, const std::string& device_id) const;
    void recordLearned(const Rect& roi, const std::string& device_id);

    // フレームセンター付近のROIを生成（低信頼度時の学習対象領域）
    static Rect computeCandidateRoi(int frame_w, int frame_h);

    // タイムスタンプ文字列生成
    static std::string nowStamp();

    CLv2Config config_;
    mutable std::mutex mutex_;
    bool running_ = false;
    Stats stats_;

    // クールダウン管理: device_id -> [(rect, frame_count)]
    std::unordered_map<std::string, std::vector<std::pair<Rect, int>>> cooldown_map_;
    int frame_counter_ = 0;

    // FrameReadyEvent購読（フレームキャッシュ用）
    SubscriptionHandle frame_sub_;

    // フレームキャッシュ（最新フレームを保持）
    struct FrameCache {
        std::string device_id;
        std::shared_ptr<mirage::SharedFrame> shared_frame;
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        const uint8_t* data() const {
            return shared_frame ? shared_frame->data() : (rgba.empty() ? nullptr : rgba.data());
        }
    };
    std::unordered_map<std::string, FrameCache> frame_cache_;
};

} // namespace mirage::ai
