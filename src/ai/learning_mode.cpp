// =============================================================================
// MirageSystem - Learning Mode 実装
// =============================================================================
// MirageComplete からの移行:
//   - D3D11テクスチャ → EventBus FrameReadyEvent (RGBA バッファ)
//   - printf/cout → MLOG_*
//   - template_writer / template_manifest 経由で保存・登録
// =============================================================================

#include "learning_mode.hpp"
#include "template_writer.hpp"
#include "../mirage_log.hpp"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>

namespace fs = std::filesystem;

namespace mirage::ai {

// =============================================================================
// タイムスタンプ生成
// =============================================================================
std::string LearningMode::nowStamp() {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    std::ostringstream ss;
    ss << ms;
    return ss.str();
}

// =============================================================================
// RGBA → Gray8 ROI切出し
// =============================================================================
Gray8 LearningMode::extractRoiGray8(
    const uint8_t* rgba_data, int frame_w, int frame_h,
    int roi_x, int roi_y, int roi_w, int roi_h)
{
    Gray8 img;

    // ROIをフレーム内にクランプ
    int x0 = std::max(0, std::min(roi_x, frame_w));
    int y0 = std::max(0, std::min(roi_y, frame_h));
    int x1 = std::max(0, std::min(roi_x + roi_w, frame_w));
    int y1 = std::max(0, std::min(roi_y + roi_h, frame_h));

    img.w = x1 - x0;
    img.h = y1 - y0;
    if (img.w <= 0 || img.h <= 0) return img;

    img.stride = img.w;
    img.pix.resize(static_cast<size_t>(img.stride) * img.h);

    // RGBA → Gray (BT.601 luminance)
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* row = rgba_data + (static_cast<size_t>(y0 + y) * frame_w + x0) * 4;
        uint8_t* dst = img.pix.data() + static_cast<size_t>(y) * img.stride;
        for (int x = 0; x < img.w; ++x) {
            // Y = 0.299R + 0.587G + 0.114B （整数近似: (77R + 150G + 29B) >> 8）
            int r = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int b = row[x * 4 + 2];
            dst[x] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
        }
    }

    return img;
}

// =============================================================================
// コンストラクタ / デストラクタ
// =============================================================================
LearningMode::LearningMode(const LearnConfig& cfg)
    : config_(cfg) {}

LearningMode::~LearningMode() {
    stop();
}

// =============================================================================
// start / stop — EventBus購読管理
// =============================================================================
void LearningMode::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;

    // FrameReadyEvent を購読してフレームキャッシュを更新
    frame_sub_ = mirage::bus().subscribe<FrameReadyEvent>(
        [this](const FrameReadyEvent& e) {
            if (!e.rgba_data || e.width <= 0 || e.height <= 0) return;
            std::lock_guard<std::mutex> lock(mutex_);
            auto& cache = frame_cache_[e.device_id];
            size_t sz = static_cast<size_t>(e.width) * e.height * 4;
            cache.device_id = e.device_id;
            cache.width = e.width;
            cache.height = e.height;
            cache.frame_id = e.frame_id;
            cache.rgba.assign(e.rgba_data, e.rgba_data + sz);
        });

    // LearningStartEvent を購読してテンプレート学習を実行
    learn_sub_ = mirage::bus().subscribe<LearningStartEvent>(
        [this](const LearningStartEvent& e) {
            onLearningStart(e);
        });

    running_ = true;
    MLOG_INFO("learning", "LearningMode 開始 (templates_dir=%s)", config_.templates_dir.c_str());
}

void LearningMode::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;

    // SubscriptionHandle の破棄で自動 unsubscribe
    frame_sub_ = {};
    learn_sub_ = {};
    frame_cache_.clear();
    running_ = false;
    MLOG_INFO("learning", "LearningMode 停止");
}

bool LearningMode::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void LearningMode::set_config(const LearnConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = cfg;
}

LearnConfig LearningMode::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

// =============================================================================
// LearningStartEvent ハンドラ
// =============================================================================
void LearningMode::onLearningStart(const LearningStartEvent& e) {
    MLOG_INFO("learning", "テンプレート学習開始: device=%s name=%s roi=(%d,%d,%d,%d)",
              e.device_id.c_str(), e.name_stem.c_str(),
              e.roi_x, e.roi_y, e.roi_w, e.roi_h);

    auto result = learnFromCachedFrame(
        e.device_id, e.name_stem,
        e.roi_x, e.roi_y, e.roi_w, e.roi_h);

    // 結果を LearningCaptureEvent として publish
    LearningCaptureEvent cap;
    cap.ok = result.ok;
    cap.error = result.error;
    cap.device_id = e.device_id;
    cap.name_stem = e.name_stem;
    cap.template_id = result.template_id;
    cap.w = result.w;
    cap.h = result.h;
    cap.saved_file_rel = result.saved_file_rel;

    mirage::bus().publish(cap);

    if (result.ok) {
        MLOG_INFO("learning", "テンプレート保存完了: id=%d file=%s (%dx%d)",
                  result.template_id, result.saved_file_rel.c_str(), result.w, result.h);
    } else {
        MLOG_ERROR("learning", "テンプレート学習失敗: %s", result.error.c_str());
    }
}

// =============================================================================
// learnFromCachedFrame — メイン処理
// フレームROI切出し → Gray8テンプレート保存 → manifest登録
// =============================================================================
LearnResult LearningMode::learnFromCachedFrame(
    const std::string& device_id,
    const std::string& name_stem,
    int roi_x, int roi_y, int roi_w, int roi_h)
{
    LearnResult r;

    if (name_stem.empty()) {
        r.error = "name_stem empty";
        return r;
    }

    // 1) キャッシュからフレーム取得
    FrameCache cached;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = frame_cache_.find(device_id);
        if (it == frame_cache_.end() || it->second.rgba.empty()) {
            r.error = "no cached frame for device: " + device_id;
            return r;
        }
        cached = it->second;
    }

    // 2) RGBA → Gray8 ROI切出し
    auto gray = extractRoiGray8(
        cached.rgba.data(), cached.width, cached.height,
        roi_x, roi_y, roi_w, roi_h);

    if (gray.w <= 0 || gray.h <= 0) {
        r.error = "ROI extraction failed (empty result)";
        return r;
    }
    r.w = gray.w;
    r.h = gray.h;

    // 3) templates ディレクトリ確保
    LearnConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = config_;
    }

    std::error_code ec;
    fs::create_directories(cfg.templates_dir, ec);

    // 4) ファイル名生成
    std::string file = name_stem;
    if (cfg.add_timestamp) {
        file += "_" + nowStamp();
    }
    file += ".png";

    std::string full_path = cfg.templates_dir + "/" + file;

    // 5) Gray8 PNG 書き出し（stb_image_write経由）
    auto wr = writeGray8Png(full_path, gray);
    if (wr.is_err()) {
        r.error = "write png failed: " + wr.error().message;
        return r;
    }

    // 6) manifest ロード → エントリ追加 → 保存
    TemplateManifest manifest;
    std::string manifest_err;
    loadManifestJson(cfg.manifest_path, manifest, &manifest_err);
    // ファイルが存在しなくても新規作成するので、エラーは無視

    int new_id = allocateNextId(manifest);

    TemplateEntry entry;
    entry.template_id = new_id;
    entry.name = name_stem;
    entry.file = file;
    entry.w = gray.w;
    entry.h = gray.h;
    entry.mtime_utc = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    manifest.entries.push_back(std::move(entry));

    if (!saveManifestJson(cfg.manifest_path, manifest, &manifest_err)) {
        r.error = "manifest save failed: " + manifest_err;
        return r;
    }

    r.ok = true;
    r.template_id = new_id;
    r.saved_file_rel = file;
    return r;
}

} // namespace mirage::ai
