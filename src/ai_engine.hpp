// =============================================================================
// MirageVulkan - AI Engine
// =============================================================================
// テンプレートマッチング + OCR 統合エンジン。
// Vulkan一本化（OpenCL/D3D11廃止）。
// EventBus連携: FrameReadyEvent受信 → processFrame → 結果をEventBus発火。
// =============================================================================

#pragma once
#include <unordered_map>

#include "result.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

// Forward declare VulkanContext
namespace mirage::vk { class VulkanContext; }

// Forward declare FrameAnalyzer（MIRAGE_OCR_ENABLED非依存）
namespace mirage { class FrameAnalyzer; }

namespace mirage::ai {

// Forward declare TemplateStore
class TemplateStore;

// =============================================================================
// 設定・構造体
// =============================================================================

struct AIConfig {
    std::string templates_dir = "templates";
    float default_threshold = 0.80f;
    bool enable_multi_scale = true;
    int max_idle_frames = 300;
    bool subscribe_events = true;  // EventBus自動購読
    // 改善L: マルチスロット遅延ジッター
    int  jitter_max_ms    = 0;   // 0=無効, >0 で [0, jitter_max_ms] のランダム遅延
    int  jitter_min_ms    = 0;   // 最小遅延（jitter_max_ms > 0 時有効）
    // 改善M: ホットリロード
    bool hot_reload           = false;
    int  hot_reload_interval_ms = 1000;
};

struct AIAction {
    enum class Type {
        NONE,
        TAP,
        SWIPE,
        BACK,
        WAIT
    };

    Type type = Type::NONE;
    int x = 0;
    int y = 0;
    int x2 = 0;  // swipe用
    int y2 = 0;
    int duration_ms = 100;

    std::string template_id;
    float confidence = 0.0f;
    std::string reason;
};

// 改善K: テンプレート別ヒット率統計
struct TemplateStats {
    uint64_t detect_count = 0;  // 検出回数（閾値超え）
    uint64_t action_count = 0;  // アクション実行回数
    uint64_t skip_count   = 0;  // COOLDOWN等でスキップされた回数
    float    hit_rate() const {
        auto total = detect_count + skip_count;
        return total > 0 ? (float)detect_count / total : 0.0f;
    }
    float    action_rate() const {
        return detect_count > 0 ? (float)action_count / detect_count : 0.0f;
    }
};

struct AIStats {
    uint64_t frames_processed = 0;
    uint64_t actions_executed = 0;
    double avg_process_time_ms = 0;
    int templates_loaded = 0;
    int idle_frames = 0;
    // 改善K: テンプレート別統計
    std::unordered_map<std::string, TemplateStats> template_stats;
};

// アクション実行コールバック
using ActionCallback = std::function<void(int slot, const AIAction& action)>;

// =============================================================================
// AIEngine
// =============================================================================

class AIEngine {
public:
    AIEngine();
    ~AIEngine();

    // 初期化 — VulkanContext必須
    mirage::Result<void> initialize(const AIConfig& config,
                                    mirage::vk::VulkanContext* vk_ctx);
    void shutdown();

    // TemplateStore接続（外部所有、non-owning）
    void setTemplateStore(TemplateStore* store);

    // FrameAnalyzer(OCR)接続（外部所有、non-owning、nullptr可）
    void setFrameAnalyzer(mirage::FrameAnalyzer* analyzer);

    // テンプレート管理
    mirage::Result<void> loadTemplatesFromDir(const std::string& dir);
    bool addTemplate(const std::string& name, const uint8_t* rgba, int w, int h);
    void clearTemplates();

    // フレーム処理（手動呼び出し用、EventBus経由でも自動呼び出しされる）
    AIAction processFrame(int slot, const uint8_t* rgba, int width, int height);

    // コールバック設定
    void setActionCallback(ActionCallback cb) { action_callback_ = cb; }
    using CanSendCallback = std::function<bool()>;
    void setCanSendCallback(CanSendCallback cb) { can_send_callback_ = cb; }

    // オーバーレイ表示用マッチ結果
    struct MatchRect {
        std::string template_id;
        std::string label;
        int x = 0, y = 0;
        int w = 0, h = 0;       // テンプレートサイズ
        int center_x = 0;       // x + w/2
        int center_y = 0;       // y + h/2
        float score = 0.0f;
    };
    std::vector<MatchRect> getLastMatches() const;

    // 統計
    AIStats getStats() const;
    void resetStats();

    // 制御
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }
    void reset();

    // VisionDecisionEngine GUI用アクセサ
    // デバイスの現在状態を取得（device_id = "slot_N"）
    int getDeviceVisionState(const std::string& device_id) const;  // VisionState as int
    void resetDeviceVision(const std::string& device_id);
    void resetAllVision();

    // VisionDecisionConfig 読み書き
    struct VDEConfig {
        int   confirm_count      = 3;
        int   cooldown_ms        = 2000;
        int   debounce_window_ms = 500;
        int   error_recovery_ms  = 3000;
        // 改善D: EWMA
        bool  enable_ewma        = false;
        float ewma_alpha         = 0.40f;
        float ewma_confirm_thr   = 0.60f;
    };
    VDEConfig getVDEConfig() const;
    // VisionDecisionConfig の変更（GUIスライダー等から呼ぶ）
    void setVDEConfig(const VDEConfig& cfg);
    // 改善L: ジッター設定 (0,0 で無効)
    void setJitterConfig(int min_ms, int max_ms);
    // 改善N: OCRキーワードマッピング
    void registerOcrKeyword(const std::string& keyword, const std::string& action);
    void removeOcrKeyword(const std::string& keyword);
    std::vector<std::pair<std::string,std::string>> getOcrKeywords() const;
    void setHotReload(bool enable, int interval_ms = 1000);

    // 全デバイスの状態一覧 (device_id → VisionState as int)
    std::vector<std::pair<std::string, int>> getAllDeviceVisionStates() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    ActionCallback action_callback_;
    CanSendCallback can_send_callback_;
    bool enabled_ = true;
};

} // namespace mirage::ai
