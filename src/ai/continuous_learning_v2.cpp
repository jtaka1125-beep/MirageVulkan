// =============================================================================
// MirageSystem - Continuous Learning v2 実装
// =============================================================================
// Layer1マッチ結果の自動監視 → 低信頼度時のテンプレート自動学習。
// 学習フロー:
//   1. onLayer1Result() で best_score < threshold を検出
//   2. クールダウン・dedup チェック
//   3. フレームキャッシュから ROI 切出し → Gray8 PNG 保存
//   4. manifest 登録（template_hot_reload 経由ではなく直接保存）
// =============================================================================

#include "continuous_learning_v2.hpp"
#include "template_writer.hpp"
#include "../mirage_log.hpp"

#include <filesystem>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

static constexpr const char* TAG = "CLv2";

namespace mirage::ai {

// =============================================================================
// タイムスタンプ生成
// =============================================================================
std::string ContinuousLearningV2::nowStamp() {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    std::ostringstream ss;
    ss << ms;
    return ss.str();
}

// =============================================================================
// フレームセンター付近のROI生成
// =============================================================================
Rect ContinuousLearningV2::computeCandidateRoi(int frame_w, int frame_h) {
    // フレーム中央の1/4領域をデフォルト学習ROIとする
    Rect roi;
    roi.w = frame_w / 4;
    roi.h = frame_h / 4;
    roi.x = (frame_w - roi.w) / 2;
    roi.y = (frame_h - roi.h) / 2;
    return roi;
}

// =============================================================================
// コンストラクタ / デストラクタ
// =============================================================================
ContinuousLearningV2::ContinuousLearningV2(const CLv2Config& cfg)
    : config_(cfg) {}

ContinuousLearningV2::~ContinuousLearningV2() {
    stop();
}

// =============================================================================
// start / stop
// =============================================================================
void ContinuousLearningV2::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;

    // FrameReadyEvent を購読してフレームキャッシュを更新
    frame_sub_ = mirage::bus().subscribe<FrameReadyEvent>(
        [this](const FrameReadyEvent& e) {
            std::lock_guard<std::mutex> lk(mutex_);
            auto& cache = frame_cache_[e.device_id];
            cache.device_id = e.device_id;
            if (e.frame) {
                cache.shared_frame = e.frame;
                cache.width  = e.frame->width;
                cache.height = e.frame->height;
                cache.rgba.clear();
            } else {
                if (!e.rgba_data || e.width <= 0 || e.height <= 0) return;
                size_t sz = static_cast<size_t>(e.width) * e.height * 4;
                cache.shared_frame.reset();
                cache.width  = e.width;
                cache.height = e.height;
                cache.rgba.assign(e.rgba_data, e.rgba_data + sz);
            }
        });

    running_ = true;
    MLOG_INFO(TAG, "ContinuousLearningV2 開始 (threshold=%.2f cooldown=%d max=%d dir=%s)",
              config_.confidence_threshold, config_.cooldown_frames,
              config_.max_templates_total, config_.templates_dir.c_str());
}

void ContinuousLearningV2::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;

    frame_sub_ = {};
    frame_cache_.clear();
    running_ = false;
    MLOG_INFO(TAG, "ContinuousLearningV2 停止 (learned=%d candidates=%d dedup=%d cooldown=%d)",
              stats_.total_learned, stats_.total_candidates,
              stats_.total_skipped_dedup, stats_.total_skipped_cooldown);
}

bool ContinuousLearningV2::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void ContinuousLearningV2::set_config(const CLv2Config& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = cfg;
}

CLv2Config ContinuousLearningV2::get_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

ContinuousLearningV2::Stats ContinuousLearningV2::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// =============================================================================
// dedup チェック: 既存クールダウンマップのROIとIoU比較
// =============================================================================
bool ContinuousLearningV2::isDuplicate(const Rect& roi, const std::string& device_id) const {
    // クールダウンマップ内の最近学習したROIと比較
    auto it = cooldown_map_.find(device_id);
    if (it == cooldown_map_.end()) return false;

    for (const auto& [learned_roi, _frame] : it->second) {
        float iou = calcIoU(roi, learned_roi);
        if (iou > 0.5f) return true;

        float dist = centerDist(roi, learned_roi);
        if (dist < 20.0f) return true;
    }
    return false;
}

// =============================================================================
// クールダウンチェック
// =============================================================================
bool ContinuousLearningV2::isInCooldown(const Rect& roi, const std::string& device_id) const {
    auto it = cooldown_map_.find(device_id);
    if (it == cooldown_map_.end()) return false;

    for (const auto& [learned_roi, learned_frame] : it->second) {
        float iou = calcIoU(roi, learned_roi);
        if (iou > 0.3f) {
            int elapsed = frame_counter_ - learned_frame;
            if (elapsed < config_.cooldown_frames) return true;
        }
    }
    return false;
}

void ContinuousLearningV2::recordLearned(const Rect& roi, const std::string& device_id) {
    auto& entries = cooldown_map_[device_id];

    // 古いエントリを削除（クールダウン期間の2倍を超えたもの）
    int expire_threshold = config_.cooldown_frames * 2;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
            [&](const std::pair<Rect, int>& p) {
                return (frame_counter_ - p.second) > expire_threshold;
            }),
        entries.end());

    entries.push_back({roi, frame_counter_});
}

// =============================================================================
// onLayer1Result — メイン判断ロジック
// =============================================================================
void ContinuousLearningV2::onLayer1Result(
    float best_score,
    const std::string& device_id,
    int frame_w, int frame_h,
    const uint8_t* rgba_data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !config_.enabled) return;

    ++frame_counter_;

    // 信頼度が閾値以上なら学習不要
    if (best_score >= config_.confidence_threshold) return;

    ++stats_.total_candidates;

    // フレームデータが必要
    const uint8_t* frame_data = rgba_data;
    int w = frame_w;
    int h = frame_h;

    // rgba_data が null の場合、フレームキャッシュから取得
    if (!frame_data) {
        auto cache_it = frame_cache_.find(device_id);
        if (cache_it == frame_cache_.end() || !cache_it->second.data()) {
            return; // フレームデータなし
        }
        frame_data = cache_it->second.data();
        w = cache_it->second.width;
        h = cache_it->second.height;
    }

    if (!frame_data || w <= 0 || h <= 0) return;

    // 学習候補ROI生成
    Rect roi = computeCandidateRoi(w, h);

    // 最小ROIサイズチェック
    if (roi.w < config_.min_roi_size || roi.h < config_.min_roi_size) return;

    // クールダウンチェック
    if (isInCooldown(roi, device_id)) {
        ++stats_.total_skipped_cooldown;
        return;
    }

    // dedup チェック
    if (isDuplicate(roi, device_id)) {
        ++stats_.total_skipped_dedup;
        return;
    }

    // テンプレート上限チェック
    TemplateManifest manifest;
    std::string manifest_err;
    loadManifestJson(config_.manifest_path, manifest, &manifest_err);

    if (static_cast<int>(manifest.entries.size()) >= config_.max_templates_total) {
        MLOG_WARN(TAG, "テンプレート上限到達 (%d/%d) — 学習スキップ",
                  static_cast<int>(manifest.entries.size()), config_.max_templates_total);
        return;
    }

    // templates/auto ディレクトリ確保
    std::error_code ec;
    fs::create_directories(config_.templates_dir, ec);

    // RGBA → Gray8 ROI切出し
    Gray8 gray;
    {
        int x0 = std::max(0, std::min(roi.x, w));
        int y0 = std::max(0, std::min(roi.y, h));
        int x1 = std::max(0, std::min(roi.x + roi.w, w));
        int y1 = std::max(0, std::min(roi.y + roi.h, h));

        gray.w = x1 - x0;
        gray.h = y1 - y0;
        if (gray.w <= 0 || gray.h <= 0) return;

        gray.stride = gray.w;
        gray.pix.resize(static_cast<size_t>(gray.stride) * gray.h);

        for (int y = 0; y < gray.h; ++y) {
            const uint8_t* row = frame_data + (static_cast<size_t>(y0 + y) * w + x0) * 4;
            uint8_t* dst = gray.pix.data() + static_cast<size_t>(y) * gray.stride;
            for (int x = 0; x < gray.w; ++x) {
                int r = row[x * 4 + 0];
                int g = row[x * 4 + 1];
                int b = row[x * 4 + 2];
                dst[x] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
            }
        }
    }

    // ファイル名生成・保存
    std::string file = "clv2_" + device_id + "_" + nowStamp() + ".png";
    std::string full_path = config_.templates_dir + "/" + file;

    auto wr = writeGray8Png(full_path, gray);
    if (wr.is_err()) {
        MLOG_ERROR(TAG, "PNG保存失敗: %s", wr.error().message.c_str());
        return;
    }

    // manifest 登録
    int new_id = allocateNextId(manifest);

    TemplateEntry entry;
    entry.template_id = new_id;
    entry.name = "clv2_auto_" + device_id;
    // templates_dir からの相対パスではなく、auto/ サブディレクトリ含むパスを格納
    entry.file = "auto/" + file;
    entry.w = gray.w;
    entry.h = gray.h;
    entry.mtime_utc = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    manifest.entries.push_back(std::move(entry));

    if (!saveManifestJson(config_.manifest_path, manifest, &manifest_err)) {
        MLOG_ERROR(TAG, "マニフェスト保存失敗: %s", manifest_err.c_str());
        return;
    }

    // クールダウン記録
    recordLearned(roi, device_id);
    ++stats_.total_learned;

    MLOG_INFO(TAG, "自動学習完了: id=%d device=%s score=%.3f file=%s (%dx%d)",
              new_id, device_id.c_str(), best_score, file.c_str(), gray.w, gray.h);
}

} // namespace mirage::ai
