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

private:
    VisionDecisionConfig config_;

    // デバイスごとの状態
    std::unordered_map<std::string, DeviceVisionState> device_states_;

    // デバウンスマップ: (device_id + template_id) → 最終実行時刻
    std::unordered_map<DebounceKey, std::chrono::steady_clock::time_point,
                       DebounceKeyHash> debounce_map_;

    // 内部ヘルパー
    DeviceVisionState& getOrCreateState(const std::string& device_id);
    void transitionTo(DeviceVisionState& ds, VisionState new_state);
    const VisionMatch* findBestMatch(const std::vector<VisionMatch>& matches) const;
    const VisionMatch* findErrorMatch(const std::vector<VisionMatch>& matches) const;
};

} // namespace mirage::ai
