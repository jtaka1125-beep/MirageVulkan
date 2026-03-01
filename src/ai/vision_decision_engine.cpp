// =============================================================================
// VisionDecisionEngine — 状態遷移マシン + デバウンス実装
// =============================================================================
// 遷移ルール:
//   IDLE → DETECTED: score > threshold
//   DETECTED → CONFIRMED: 同一テンプレートN回連続 (default 3)
//   DETECTED → IDLE: 別テンプレート or マッチなし
//   CONFIRMED → COOLDOWN: アクション実行後 (notifyActionExecuted)
//   COOLDOWN → IDLE: cooldown_ms 経過
//   ANY → ERROR_RECOVERY: errorグループテンプレート検出
//   ERROR_RECOVERY → IDLE: 回復アクション実行後
// =============================================================================

#include "vision_decision_engine.hpp"
#include "ollama_vision.hpp"
#include "mirage_log.hpp"
#include "event_bus.hpp"
#include <algorithm>

namespace mirage::ai {

// =============================================================================
// コンストラクタ
// =============================================================================

VisionDecisionEngine::VisionDecisionEngine(const VisionDecisionConfig& config)
    : config_(config) {}

// =============================================================================
// メイン更新: マッチ結果を受けて状態遷移
// =============================================================================

VisionDecision VisionDecisionEngine::update(
    const std::string& device_id,
    const std::vector<VisionMatch>& matches,
    std::chrono::steady_clock::time_point now)
{
    auto& ds = getOrCreateState(device_id);
    VisionDecision decision;
    decision.prev_state = ds.state;

    // === ANY → ERROR_RECOVERY: errorグループ最優先 ===
    const VisionMatch* error_match = findErrorMatch(matches);
    if (error_match && ds.state != VisionState::ERROR_RECOVERY) {
        VisionState old_state = ds.state;
        transitionTo(ds, VisionState::ERROR_RECOVERY);
        ds.error_start = now;

        decision.should_act = true;
        decision.is_error_recovery = true;
        decision.template_id = error_match->template_id;
        decision.x = error_match->x;
        decision.y = error_match->y;
        decision.score = error_match->score;
        decision.state = VisionState::ERROR_RECOVERY;
        decision.prev_state = old_state;

        MLOG_INFO("ai.vision", "エラーテンプレート検出 → ERROR_RECOVERY: device=%s tpl=%s",
                  device_id.c_str(), error_match->template_id.c_str());

        // StateChangeEvent発行
        mirage::StateChangeEvent evt;
        evt.device_id = device_id;
        evt.old_state = static_cast<int>(old_state);
        evt.new_state = static_cast<int>(VisionState::ERROR_RECOVERY);
        evt.template_id = error_match->template_id;
        evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        mirage::bus().publish(evt);

        return decision;
    }

    // === ERROR_RECOVERY中: タイムアウトチェック ===
    if (ds.state == VisionState::ERROR_RECOVERY) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ds.error_start).count();
        if (elapsed >= config_.error_recovery_ms) {
            VisionState old_state = ds.state;
            transitionTo(ds, VisionState::IDLE);
            MLOG_WARN("ai.vision", "ERROR_RECOVERY タイムアウト → IDLE: device=%s (%lldms)",
                      device_id.c_str(), (long long)elapsed);

            mirage::StateChangeEvent evt;
            evt.device_id = device_id;
            evt.old_state = static_cast<int>(old_state);
            evt.new_state = static_cast<int>(VisionState::IDLE);
            evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            mirage::bus().publish(evt);
        }
        // ERROR_RECOVERY中はエラー以外のアクション抑制
        // Layer3トリガーカウンタをリセット（エラー対応中は未知UIではない）
        ds.consecutive_no_match = 0;
        ds.consecutive_same_match = 0;
        ds.last_any_match_time = now;
        decision.state = ds.state;
        return decision;
    }

    // === COOLDOWN → IDLE: 冷却時間経過チェック ===
    if (ds.state == VisionState::COOLDOWN) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ds.cooldown_start).count();
        if (elapsed >= config_.cooldown_ms) {
            VisionState old_state = ds.state;
            transitionTo(ds, VisionState::IDLE);
            MLOG_DEBUG("ai.vision", "COOLDOWN完了 → IDLE: device=%s tpl=%s (%lldms)",
                       device_id.c_str(), ds.cooldown_template_id.c_str(),
                       (long long)elapsed);

            mirage::StateChangeEvent evt;
            evt.device_id = device_id;
            evt.old_state = static_cast<int>(old_state);
            evt.new_state = static_cast<int>(VisionState::IDLE);
            evt.template_id = ds.cooldown_template_id;
            evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            mirage::bus().publish(evt);

            ds.cooldown_template_id.clear();
        } else {
            // まだCOOLDOWN中 — アクション抑制
            // Layer3トリガーカウンタをリセット（冷却中は正常な待機状態）
            ds.consecutive_no_match = 0;
            ds.consecutive_same_match = 0;
            ds.last_any_match_time = now;
            decision.state = VisionState::COOLDOWN;
            return decision;
        }
    }

    // === ベストマッチ選択 ===
    const VisionMatch* best = findBestMatch(matches);

    if (!best) {
        // マッチなし — 改善D: EWMAを減衰
        if (config_.enable_ewma && !ds.ewma_template_id.empty()) {
            ds.ewma_score = (1.0f - config_.ewma_alpha) * ds.ewma_score;
        }

        // Layer 3トリガー: マッチなし継続カウント更新
        ds.consecutive_no_match++;
        ds.consecutive_same_match = 0;

        if (ds.state == VisionState::DETECTED) {
            VisionState old_state = ds.state;
            transitionTo(ds, VisionState::IDLE);
            ds.consecutive_count = 0;
            ds.detected_template_id.clear();

            MLOG_DEBUG("ai.vision", "マッチなし → IDLE: device=%s", device_id.c_str());

            mirage::StateChangeEvent evt;
            evt.device_id = device_id;
            evt.old_state = static_cast<int>(old_state);
            evt.new_state = static_cast<int>(VisionState::IDLE);
            evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            mirage::bus().publish(evt);
        }
        decision.state = ds.state;
        return decision;
    }

    // Layer 3トリガー: マッチあり時のカウンタ更新
    ds.consecutive_no_match = 0;
    ds.last_any_match_time = now;
    if (best->template_id == ds.last_matched_template) {
        ds.consecutive_same_match++;
    } else {
        ds.consecutive_same_match = 1;
        ds.last_matched_template = best->template_id;
    }

    // === デバウンスチェック ===
    if (isDebounced(device_id, best->template_id, now)) {
        decision.state = ds.state;
        return decision;
    }

    // === 改善D: EWMA 更新 ===
    // テンプレートが切り替わったらEWMAをリセット
    if (config_.enable_ewma) {
        if (ds.ewma_template_id != best->template_id) {
            ds.ewma_score = 0.0f;
            ds.ewma_template_id = best->template_id;
        }
        // 存在(1.0)方向にEWMAを更新
        ds.ewma_score = config_.ewma_alpha * 1.0f + (1.0f - config_.ewma_alpha) * ds.ewma_score;
    }

    // === 状態遷移ロジック ===
    switch (ds.state) {
        case VisionState::IDLE: {
            // IDLE → DETECTED: スコアが閾値超え
            VisionState old_state = ds.state;
            transitionTo(ds, VisionState::DETECTED);
            ds.detected_template_id = best->template_id;
            ds.consecutive_count = 1;

            MLOG_DEBUG("ai.vision", "検出開始 → DETECTED: device=%s tpl=%s score=%.3f",
                       device_id.c_str(), best->template_id.c_str(), best->score);

            mirage::StateChangeEvent evt;
            evt.device_id = device_id;
            evt.old_state = static_cast<int>(old_state);
            evt.new_state = static_cast<int>(VisionState::DETECTED);
            evt.template_id = best->template_id;
            evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            mirage::bus().publish(evt);
            break;
        }

        case VisionState::DETECTED: {
            if (best->template_id == ds.detected_template_id) {
                // 同一テンプレート連続検出
                ds.consecutive_count++;

                // 改善D: EWMAゲート（enable_ewma=trueのとき ewma_confirm_thr 以上必要）
                bool ewma_ok = !config_.enable_ewma ||
                               (ds.ewma_score >= config_.ewma_confirm_thr);

                if (ds.consecutive_count >= config_.confirm_count && ewma_ok) {
                    // DETECTED → CONFIRMED
                    VisionState old_state = ds.state;
                    transitionTo(ds, VisionState::CONFIRMED);

                    decision.should_act = true;
                    decision.template_id = best->template_id;
                    decision.x = best->x;
                    decision.y = best->y;
                    decision.score = best->score;

                    MLOG_INFO("ai.vision", "確定 → CONFIRMED: device=%s tpl=%s count=%d score=%.3f",
                              device_id.c_str(), best->template_id.c_str(),
                              ds.consecutive_count, best->score);

                    mirage::StateChangeEvent evt;
                    evt.device_id = device_id;
                    evt.old_state = static_cast<int>(old_state);
                    evt.new_state = static_cast<int>(VisionState::CONFIRMED);
                    evt.template_id = best->template_id;
                    evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
                    mirage::bus().publish(evt);
                }
            } else {
                // 別テンプレート → リセットして新テンプレートで再検出開始
                VisionState old_state = ds.state;
                ds.detected_template_id = best->template_id;
                ds.consecutive_count = 1;

                MLOG_DEBUG("ai.vision", "別テンプレート検出 → DETECTED(リセット): device=%s tpl=%s",
                           device_id.c_str(), best->template_id.c_str());

                mirage::StateChangeEvent evt;
                evt.device_id = device_id;
                evt.old_state = static_cast<int>(old_state);
                evt.new_state = static_cast<int>(VisionState::DETECTED);
                evt.template_id = best->template_id;
                evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                mirage::bus().publish(evt);
            }
            break;
        }

        case VisionState::CONFIRMED: {
            // CONFIRMED状態 — notifyActionExecuted待ち
            // アクションが既にshould_act=trueで返されている
            // 追加アクションは抑制
            break;
        }

        default:
            break;
    }

    decision.state = ds.state;
    return decision;
}

// =============================================================================
// アクション実行完了通知 → CONFIRMED → COOLDOWN
// =============================================================================

void VisionDecisionEngine::notifyActionExecuted(
    const std::string& device_id,
    std::chrono::steady_clock::time_point now)
{
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return;

    auto& ds = it->second;

    if (ds.state == VisionState::CONFIRMED) {
        VisionState old_state = ds.state;
        ds.cooldown_template_id = ds.detected_template_id;
        ds.cooldown_start = now;
        transitionTo(ds, VisionState::COOLDOWN);

        // デバウンスマップ更新
        DebounceKey key{device_id, ds.detected_template_id};
        debounce_map_[key] = now;

        MLOG_DEBUG("ai.vision", "アクション実行完了 → COOLDOWN: device=%s tpl=%s",
                   device_id.c_str(), ds.cooldown_template_id.c_str());

        mirage::StateChangeEvent evt;
        evt.device_id = device_id;
        evt.old_state = static_cast<int>(old_state);
        evt.new_state = static_cast<int>(VisionState::COOLDOWN);
        evt.template_id = ds.cooldown_template_id;
        evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        mirage::bus().publish(evt);

        ds.detected_template_id.clear();
        ds.consecutive_count = 0;
    } else if (ds.state == VisionState::ERROR_RECOVERY) {
        // エラー回復アクション実行完了 → IDLE
        VisionState old_state = ds.state;
        transitionTo(ds, VisionState::IDLE);

        MLOG_INFO("ai.vision", "エラー回復完了 → IDLE: device=%s", device_id.c_str());

        mirage::StateChangeEvent evt;
        evt.device_id = device_id;
        evt.old_state = static_cast<int>(old_state);
        evt.new_state = static_cast<int>(VisionState::IDLE);
        evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        mirage::bus().publish(evt);

        ds.detected_template_id.clear();
        ds.consecutive_count = 0;
    }
}

// =============================================================================
// リセット
// =============================================================================

void VisionDecisionEngine::resetDevice(const std::string& device_id) {
    device_states_.erase(device_id);
    // デバウンスマップからこのデバイスのエントリを削除
    for (auto it = debounce_map_.begin(); it != debounce_map_.end(); ) {
        if (it->first.device_id == device_id)
            it = debounce_map_.erase(it);
        else
            ++it;
    }
}

void VisionDecisionEngine::resetAll() {
    device_states_.clear();
    debounce_map_.clear();
}

// =============================================================================
// 状態クエリ
// =============================================================================

VisionState VisionDecisionEngine::getDeviceState(const std::string& device_id) const {
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return VisionState::IDLE;
    return it->second.state;
}

bool VisionDecisionEngine::isDebounced(
    const std::string& device_id,
    const std::string& template_id,
    std::chrono::steady_clock::time_point now) const
{
    DebounceKey key{device_id, template_id};
    auto it = debounce_map_.find(key);
    if (it == debounce_map_.end()) return false;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - it->second).count();
    return elapsed < config_.debounce_window_ms;
}

// =============================================================================
// 内部ヘルパー
// =============================================================================

DeviceVisionState& VisionDecisionEngine::getOrCreateState(const std::string& device_id) {
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) {
        // 新規デバイス: last_any_match_time を now で初期化して起動即トリガーを防ぐ
        DeviceVisionState& ds = device_states_[device_id];
        ds.last_any_match_time = std::chrono::steady_clock::now();
        ds.layer3_last_call   = std::chrono::steady_clock::now();
        return ds;
    }
    return it->second;
}

void VisionDecisionEngine::transitionTo(DeviceVisionState& ds, VisionState new_state) {
    ds.state = new_state;
}

const VisionMatch* VisionDecisionEngine::findBestMatch(
    const std::vector<VisionMatch>& matches) const
{
    const VisionMatch* best = nullptr;
    for (const auto& m : matches) {
        if (m.is_error_group) continue;  // errorグループは別処理
        if (!best || m.score > best->score) {
            best = &m;
        }
    }
    return best;
}

const VisionMatch* VisionDecisionEngine::findErrorMatch(
    const std::vector<VisionMatch>& matches) const
{
    for (const auto& m : matches) {
        if (m.is_error_group) return &m;
    }
    return nullptr;
}

// =============================================================================
// Layer 3: OllamaVision 非同期統合
// =============================================================================

void VisionDecisionEngine::setOllamaVision(std::shared_ptr<OllamaVision> ollama) {
    ollama_vision_ = std::move(ollama);
}

bool VisionDecisionEngine::isLayer3OnCooldown(
    const std::string& device_id,
    std::chrono::steady_clock::time_point now) const
{
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return false;

    const auto& ds = it->second;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ds.layer3_last_call).count();
    return elapsed < config_.layer3_cooldown_ms;
}

bool VisionDecisionEngine::isLayer3Running(const std::string& device_id) const {
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return false;

    const auto& ds = it->second;
    return ds.layer3_task && ds.layer3_task->valid;
}

bool VisionDecisionEngine::launchLayer3Async(
    const std::string& device_id,
    const uint8_t* rgba, int width, int height,
    std::chrono::steady_clock::time_point now)
{
    // Layer 3無効 or OllamaVision未設定
    if (!config_.enable_layer3 || !ollama_vision_) {
        return false;
    }

    // 既に実行中
    if (isLayer3Running(device_id)) {
        MLOG_DEBUG("ai.vision", "Layer 3既に実行中: device=%s", device_id.c_str());
        return false;
    }

    // 冷却中チェック
    if (isLayer3OnCooldown(device_id, now)) {
        MLOG_DEBUG("ai.vision", "Layer 3冷却中: device=%s", device_id.c_str());
        return false;
    }

    // グローバル排他: 全デバイス合計で LAYER3_MAX_CONCURRENT まで
    if (layer3_active_count_.load() >= LAYER3_MAX_CONCURRENT) {
        MLOG_DEBUG("ai.vision", "Layer 3グローバル上限: device=%s active=%d",
                   device_id.c_str(), layer3_active_count_.load());
        return false;
    }

    auto& ds = getOrCreateState(device_id);
    ds.layer3_last_call = now;

    // RGBAデータをコピー（非同期タスク用）
    size_t data_size = static_cast<size_t>(width) * height * 4;
    auto rgba_copy = std::make_shared<std::vector<uint8_t>>(rgba, rgba + data_size);

    // 非同期タスク作成
    auto task = std::make_shared<Layer3Task>();
    task->start_time = now;
    task->frame_width = width;
    task->frame_height = height;
    task->valid = true;

    // OllamaVisionのshared_ptrをキャプチャ
    auto ollama = ollama_vision_;

    layer3_active_count_++;
    auto* active_count = &layer3_active_count_;
    task->future = std::async(std::launch::async,
        [ollama, rgba_copy, width, height, active_count]() -> OllamaVisionResult {
            OllamaVisionResult r = ollama->detectPopup(rgba_copy->data(), width, height);
            (*active_count)--;
            return r;
        });

    ds.layer3_task = task;

    MLOG_INFO("ai.vision", "Layer 3非同期起動: device=%s %dx%d",
              device_id.c_str(), width, height);

    return true;
}

VisionDecisionEngine::Layer3Result VisionDecisionEngine::pollLayer3Result(
    const std::string& device_id)
{
    Layer3Result result;
    result.has_result = false;

    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return result;

    auto& ds = it->second;
    if (!ds.layer3_task || !ds.layer3_task->valid) return result;

    // 完了チェック（ノンブロッキング）
    auto status = ds.layer3_task->future.wait_for(std::chrono::seconds(0));
    if (status != std::future_status::ready) {
        return result;  // まだ実行中
    }

    // 完了 — 結果を取得
    result.has_result = true;

    try {
        OllamaVisionResult ollama_result = ds.layer3_task->future.get();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ds.layer3_task->start_time).count();
        result.elapsed_ms = static_cast<int>(elapsed);

        if (!ollama_result.error.empty()) {
            result.error = ollama_result.error;
            MLOG_WARN("ai.vision", "Layer 3エラー: device=%s error=%s",
                      device_id.c_str(), result.error.c_str());
        } else if (ollama_result.found) {
            result.found = true;
            result.type = ollama_result.type;
            result.button_text = ollama_result.button_text;
            // パーセント座標をピクセルに変換
            result.x = (ollama_result.x_percent * ds.layer3_task->frame_width) / 100;
            result.y = (ollama_result.y_percent * ds.layer3_task->frame_height) / 100;

            MLOG_INFO("ai.vision", "Layer 3検出成功: device=%s type=%s button='%s' pos=(%d,%d) (%dms)",
                      device_id.c_str(), result.type.c_str(), result.button_text.c_str(),
                      result.x, result.y, result.elapsed_ms);
        } else {
            MLOG_DEBUG("ai.vision", "Layer 3: ポップアップ検出なし (%dms)",
                       result.elapsed_ms);
        }
    } catch (const std::exception& e) {
        result.error = e.what();
        MLOG_ERROR("ai.vision", "Layer 3例外: device=%s error=%s",
                   device_id.c_str(), e.what());
    }

    // タスク完了 — 無効化
    ds.layer3_task->valid = false;
    ds.layer3_task.reset();

    return result;
}

void VisionDecisionEngine::cancelLayer3(const std::string& device_id) {
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return;

    auto& ds = it->second;
    if (ds.layer3_task && ds.layer3_task->valid) {
        MLOG_DEBUG("ai.vision", "Layer 3キャンセル: device=%s", device_id.c_str());
        ds.layer3_task->valid = false;
        // futureは破棄されるがスレッドは走り続ける（ラムダ内で -1 される）
        ds.layer3_task.reset();
    }
}


bool VisionDecisionEngine::shouldTriggerLayer3(
    const std::string& device_id,
    std::chrono::steady_clock::time_point now) const
{
    if (!config_.enable_layer3) return false;

    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return false;
    const auto& ds = it->second;

    // 既に Layer 3 実行中または冷却中はスキップ
    if (ds.layer3_task && ds.layer3_task->valid) return false;
    if (isLayer3OnCooldown(device_id, now)) return false;
    // グローバル上限チェック
    if (layer3_active_count_.load() >= LAYER3_MAX_CONCURRENT) return false;

    // ① 連続マッチなしフレーム数トリガー
    if (config_.layer3_no_match_frames > 0 &&
        ds.consecutive_no_match >= config_.layer3_no_match_frames) {
        MLOG_DEBUG("ai.vision", "Layer3トリガー(no_match_frames): device=%s count=%d",
                   device_id.c_str(), ds.consecutive_no_match);
        return true;
    }

    // ② 時間ベーストリガー (フレームレート非依存)
    if (config_.layer3_no_match_ms > 0 && ds.consecutive_no_match > 0) {
        auto since_last_match = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ds.last_any_match_time).count();
        if (since_last_match >= config_.layer3_no_match_ms) {
            MLOG_DEBUG("ai.vision", "Layer3トリガー(no_match_ms): device=%s elapsed=%lldms",
                       device_id.c_str(), (long long)since_last_match);
            return true;
        }
    }

    // ③ 同一テンプレート貼り付きトリガー（タップが効いていない疑い）
    if (config_.layer3_stuck_frames > 0 &&
        ds.consecutive_same_match >= config_.layer3_stuck_frames) {
        MLOG_DEBUG("ai.vision", "Layer3トリガー(stuck): device=%s tpl=%s count=%d",
                   device_id.c_str(), ds.last_matched_template.c_str(),
                   ds.consecutive_same_match);
        return true;
    }

    return false;
}

} // namespace mirage::ai
