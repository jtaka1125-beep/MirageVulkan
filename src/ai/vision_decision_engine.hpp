// =============================================================================
// VisionDecisionEngine — 状態遷移マシン + デバウンス強化
// =============================================================================
// テンプレートマッチング結果を受けて、状態遷移に基づきアクション実行を判断。
// IDLE → DETECTED → CONFIRMED → COOLDOWN のサイクルで誤検出・連打を防止。
// ERROR_RECOVERY: エラーポップアップ検出時の自動回復。
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <memory>
#include <functional>
#include <future>
#include <atomic>

#include "ollama_vision.hpp"

namespace mirage::ai {

// =============================================================================
// 状態定義
// =============================================================================

enum class VisionState {
    IDLE,             // マッチなし
    DETECTED,         // テンプレート検出、確認待ち（N回連続で確定）
    CONFIRMED,        // 確定、アクション実行可
    COOLDOWN,         // 実行後冷却（同一テンプレート再実行防止）
    ERROR_RECOVERY    // エラーポップアップ検出時の自動回復
};

inline const char* visionStateToString(VisionState s) {
    switch (s) {
        case VisionState::IDLE:           return "IDLE";
        case VisionState::DETECTED:       return "DETECTED";
        case VisionState::CONFIRMED:      return "CONFIRMED";
        case VisionState::COOLDOWN:       return "COOLDOWN";
        case VisionState::ERROR_RECOVERY: return "ERROR_RECOVERY";
    }
    return "UNKNOWN";
}

// =============================================================================
// 設定
// =============================================================================

struct VisionDecisionConfig {
    int confirm_count = 3;           // DETECTED→CONFIRMED に必要な連続検出回数
    int cooldown_ms = 2000;          // COOLDOWN→IDLE までの冷却時間(ms)
    int debounce_window_ms = 500;    // デバウンスウィンドウ(ms)
    int error_recovery_ms = 3000;    // ERROR_RECOVERY最大滞在時間(ms)
    // ── 改善D: Temporal Consistency Filter ──
    bool  enable_ewma      = false;  // EWMA平滑化 ON/OFF（デフォルトOFF=後方互換）
    float ewma_alpha       = 0.40f;  // 平滑化係数 (0=強平滑, 1=生スコアそのまま)
    float ewma_confirm_thr = 0.60f;  // EWMA が この値以上でないと CONFIRMED 不可
    // ── Layer 3: Ollama LLM Vision ──
    bool enable_layer3 = false;      // Layer 3 (LLM Vision) を有効化
    int layer3_cooldown_ms = 30000;  // Layer 3 呼び出し後の冷却時間(ms) - LLMは重いので長め
    // ── Layer 3 トリガー閾値 ──
    int layer3_no_match_frames = 150; // 連続マッチなしフレーム数 (~5秒@30fps)
    int layer3_stuck_frames    = 300; // 同一テンプレート継続フレーム数 (~10秒@30fps)
    int layer3_no_match_ms     = 5000;// 時間ベーストリガー (フレームレート非依存)
};

// =============================================================================
// マッチ入力（VulkanTemplateMatcher結果の軽量コピー）
// =============================================================================

struct VisionMatch {
    std::string template_id;   // テンプレート名
    int x = 0, y = 0;
    float score = 0.0f;
    bool is_error_group = false;  // errorグループテンプレートか
};

// =============================================================================
// 決定結果
// =============================================================================

struct VisionDecision {
    bool should_act = false;          // アクション実行すべきか
    bool is_error_recovery = false;   // エラー回復アクションか
    std::string template_id;          // 対象テンプレートID
    int x = 0, y = 0;
    float score = 0.0f;
    VisionState state = VisionState::IDLE;      // 現在の状態
    VisionState prev_state = VisionState::IDLE;  // 遷移前の状態
};

// =============================================================================
// デバウンスキー: device_id + template_id
// =============================================================================

struct DebounceKey {
    std::string device_id;
    std::string template_id;

    bool operator==(const DebounceKey& other) const {
        return device_id == other.device_id && template_id == other.template_id;
    }
};

struct DebounceKeyHash {
    size_t operator()(const DebounceKey& k) const {
        size_t h1 = std::hash<std::string>{}(k.device_id);
        size_t h2 = std::hash<std::string>{}(k.template_id);
        return h1 ^ (h2 << 32 | h2 >> 32);
    }
};

// =============================================================================
// デバイスごとの状態追跡
// =============================================================================

// Layer 3 非同期タスク状態
struct Layer3Task {
    std::future<OllamaVisionResult> future;
    std::chrono::steady_clock::time_point start_time;
    int frame_width = 0;
    int frame_height = 0;
    bool valid = false;  // タスクが有効か
};

struct DeviceVisionState {
    VisionState state = VisionState::IDLE;
    std::string detected_template_id;      // DETECTED中のテンプレートID
    int consecutive_count = 0;             // 連続検出カウント
    std::chrono::steady_clock::time_point cooldown_start;  // COOLDOWN開始時刻
    std::chrono::steady_clock::time_point error_start;     // ERROR_RECOVERY開始時刻
    std::string cooldown_template_id;      // COOLDOWN中のテンプレートID
    // ── 改善D: EWMA ──
    float ewma_score = 0.0f;              // テンプレート存在感の指数移動平均
    std::string ewma_template_id;          // EWMA追跡中のテンプレートID
    // ── Layer 3: LLM Vision (非同期) ──
    std::chrono::steady_clock::time_point layer3_last_call;  // Layer 3最終呼び出し時刻
    std::shared_ptr<Layer3Task> layer3_task;  // 実行中の非同期タスク
    // ── Layer 3 トリガー条件 ──
    int  consecutive_no_match = 0;        // 連続マッチなしフレーム数
    int  consecutive_same_match = 0;      // 同一テンプレート継続フレーム数
    std::string last_matched_template;    // 前フレームのマッチID
    std::chrono::steady_clock::time_point last_any_match_time;  // 最後にマッチした時刻
};

// =============================================================================
// VisionDecisionEngine
// =============================================================================

class VisionDecisionEngine {
public:
    explicit VisionDecisionEngine(const VisionDecisionConfig& config = {});
    ~VisionDecisionEngine() = default;

    // マッチ結果を入力して状態遷移 → 決定を返す
    VisionDecision update(const std::string& device_id,
                          const std::vector<VisionMatch>& matches,
                          std::chrono::steady_clock::time_point now =
                              std::chrono::steady_clock::now());

    // アクション実行完了通知（CONFIRMED→COOLDOWN遷移）
    void notifyActionExecuted(const std::string& device_id,
                              std::chrono::steady_clock::time_point now =
                                  std::chrono::steady_clock::now());

    // デバイス状態リセット
    void resetDevice(const std::string& device_id);

    // 全デバイスリセット
    void resetAll();

    // 設定変更
    void setConfig(const VisionDecisionConfig& config) { config_ = config; }
    const VisionDecisionConfig& config() const { return config_; }

    // デバイスの現在状態を取得
    VisionState getDeviceState(const std::string& device_id) const;

    // デバウンスチェック（外部からのクエリ用）
    bool isDebounced(const std::string& device_id,
                     const std::string& template_id,
                     std::chrono::steady_clock::time_point now =
                         std::chrono::steady_clock::now()) const;

    // ── Layer 3: LLM Vision (非同期) ──
    // OllamaVisionインスタンスを設定（外部から注入）
    void setOllamaVision(std::shared_ptr<OllamaVision> ollama);

    // Layer 3検出結果
    struct Layer3Result {
        bool has_result = false;     // 結果があるか
        bool found = false;          // ポップアップ検出したか
        std::string type;            // ad/permission/error/notification/other
        std::string button_text;     // 閉じるボタンのテキスト
        int x = 0, y = 0;            // ボタン座標（ピクセル）
        int elapsed_ms = 0;          // 処理時間
        std::string error;           // エラーメッセージ
    };

    // Layer 3を非同期で起動（fire & forget）
    // @return true: 起動成功, false: 既に実行中/冷却中/無効
    bool launchLayer3Async(const std::string& device_id,
                           const uint8_t* rgba, int width, int height,
                           std::chrono::steady_clock::time_point now =
                               std::chrono::steady_clock::now());

    // Layer 3の結果をポーリング（毎フレーム呼び出し）
    // 完了していれば結果を返す、未完了なら has_result=false
    Layer3Result pollLayer3Result(const std::string& device_id);

    // Layer 3が実行中かチェック
    bool isLayer3Running(const std::string& device_id) const;

    // Layer 3が冷却中かチェック
    bool isLayer3OnCooldown(const std::string& device_id,
                            std::chrono::steady_clock::time_point now =
                                std::chrono::steady_clock::now()) const;

    // Layer 3をキャンセル（結果を破棄）
    void cancelLayer3(const std::string& device_id);

    // Layer 3トリガー条件を満たしているか判定
    bool shouldTriggerLayer3(const std::string& device_id,
                              std::chrono::steady_clock::time_point now =
                                  std::chrono::steady_clock::now()) const;

private:
    VisionDecisionConfig config_;

    // デバイスごとの状態
    std::unordered_map<std::string, DeviceVisionState> device_states_;

    // デバウンスマップ: (device_id + template_id) → 最終実行時刻
    std::unordered_map<DebounceKey, std::chrono::steady_clock::time_point,
                       DebounceKeyHash> debounce_map_;

    // Layer 3: OllamaVision (外部注入)
    std::shared_ptr<OllamaVision> ollama_vision_;

    // Layer 3 グローバル排他: 全デバイス合計で同時1つまで
    mutable std::atomic<int> layer3_active_count_{0};
    static constexpr int LAYER3_MAX_CONCURRENT = 1; // Ollama はシングルスレッド

    // 内部ヘルパー
    DeviceVisionState& getOrCreateState(const std::string& device_id);
    void transitionTo(DeviceVisionState& ds, VisionState new_state);
    const VisionMatch* findBestMatch(const std::vector<VisionMatch>& matches) const;
    const VisionMatch* findErrorMatch(const std::vector<VisionMatch>& matches) const;
};

} // namespace mirage::ai
