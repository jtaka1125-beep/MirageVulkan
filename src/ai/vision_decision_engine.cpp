// =============================================================================
// VisionDecisionEngine — 3層状態遷移マシン
// =============================================================================
// Layer構成:
//   Layer 0 (STANDBY): 待機モード - テンプレートマッチング停止、低負荷
//   Layer 1 (IDLE/DETECTED/CONFIRMED/COOLDOWN): テンプレートマッチング稼働
//   Layer 2 (AI Vision): LLM並列投票によるポップアップ検出
//
// 遷移ルール:
//   Layer 0→1: 操作なし 5秒 (layer0_idle_timeout_ms)
//   Layer 1→0: ユーザー操作検出 (notifyUserInput)
//   Layer 1→2: 90秒マッチなし (layer2_no_match_ms)
//   Layer 2→0: フリーズ60秒 → ホームボタン → STANDBY
//
//   IDLE → DETECTED: score > threshold
//   DETECTED → CONFIRMED: 同一テンプレートN回連続 (default 3)
//   DETECTED → IDLE: 別テンプレート or マッチなし
//   CONFIRMED → COOLDOWN: アクション実行後 (notifyActionExecuted)
//   COOLDOWN → IDLE: cooldown_ms 経過
//   ANY → ERROR_RECOVERY: errorグループテンプレート検出
//   ERROR_RECOVERY → IDLE: 回復アクション実行後
// =============================================================================

#include "vision_decision_engine.hpp"
#include "mirage_log.hpp"
#include "event_bus.hpp"
#include <algorithm>
#include <fstream>
#include <regex>

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

    // === Layer 0 (STANDBY): テンプレートマッチング停止 ===
    if (ds.state == VisionState::STANDBY) {
        // STANDBYでは何もしない (テンプレートマッチングスキップ)
        // checkLayer0Timeout() で IDLE に遷移するまで待機
        decision.state = VisionState::STANDBY;
        return decision;
    }

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
        // Layer2トリガーカウンタをリセット（エラー対応中は未知UIではない）
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
            // Layer2トリガーカウンタをリセット（冷却中は正常な待機状態）
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

        // Layer 2トリガー: マッチなし継続カウント更新
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

    // Layer 2トリガー: マッチあり時のカウンタ更新
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
            decision.template_id = best->template_id;
            decision.score = best->score;

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
// アクション実行完了通知 → CONFIRMED → VERIFYING (検証有効時) or COOLDOWN
// =============================================================================

void VisionDecisionEngine::notifyActionExecuted(
    const std::string& device_id,
    int action_x, int action_y,
    std::chrono::steady_clock::time_point now)
{
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return;

    auto& ds = it->second;

    if (ds.state == VisionState::CONFIRMED) {
        VisionState old_state = ds.state;
        
        if (config_.enable_verify) {
            // 検証有効: VERIFYING状態へ遷移
            ds.verify_template_id = ds.detected_template_id;
            ds.verify_start = now;
            ds.verify_retry_count = 0;
            ds.verify_x = action_x;  // リトライ用座標を保存
            ds.verify_y = action_y;
            transitionTo(ds, VisionState::VERIFYING);

            MLOG_INFO("ai.vision", "アクション実行 → VERIFYING: device=%s tpl=%s",
                       device_id.c_str(), ds.verify_template_id.c_str());

            mirage::StateChangeEvent evt;
            evt.device_id = device_id;
            evt.old_state = static_cast<int>(old_state);
            evt.new_state = static_cast<int>(VisionState::VERIFYING);
            evt.template_id = ds.verify_template_id;
            evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            mirage::bus().publish(evt);
        } else {
            // 検証無効: 従来通りCOOLDOWNへ
            ds.cooldown_template_id = ds.detected_template_id;
            ds.cooldown_start = now;
            transitionTo(ds, VisionState::COOLDOWN);

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
        }
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
// アクション検証 (VERIFYING状態)
// =============================================================================

VisionDecisionEngine::VerifyResult VisionDecisionEngine::checkVerification(
    const std::string& device_id,
    const std::vector<VisionMatch>& matches,
    std::chrono::steady_clock::time_point now)
{
    VerifyResult result;
    
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return result;
    
    auto& ds = it->second;
    if (ds.state != VisionState::VERIFYING) return result;
    
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ds.verify_start).count();
    
    // 待機時間経過前は何もしない
    if (elapsed_ms < config_.verify_delay_ms) {
        return result;
    }
    
    // 同じテンプレートがまだ検出されているか確認
    bool still_detected = false;
    for (const auto& m : matches) {
        if (m.template_id == ds.verify_template_id) {
            still_detected = true;
            result.x = m.x;
            result.y = m.y;
            break;
        }
    }
    
    if (!still_detected) {
        // テンプレート消失 → 検証成功
        result.verified_success = true;
        
        VisionState old_state = ds.state;
        ds.cooldown_template_id = ds.verify_template_id;
        ds.cooldown_start = now;
        transitionTo(ds, VisionState::COOLDOWN);
        
        DebounceKey key{device_id, ds.verify_template_id};
        debounce_map_[key] = now;
        
        // 検証記録: 成功をカウント
        auto& vrec = verify_records_[ds.verify_template_id];
        vrec.success_count++;
        vrec.last_success = now;

        MLOG_INFO("ai.vision", "検証成功 (消失確認) → COOLDOWN: device=%s tpl=%s (success=%u fail=%u rate=%.1f%%)",
                   device_id.c_str(), ds.verify_template_id.c_str(),
                   vrec.success_count, vrec.fail_count, vrec.success_rate() * 100.0f);
        
        mirage::StateChangeEvent evt;
        evt.device_id = device_id;
        evt.old_state = static_cast<int>(old_state);
        evt.new_state = static_cast<int>(VisionState::COOLDOWN);
        evt.template_id = ds.cooldown_template_id;
        evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        mirage::bus().publish(evt);
        
        ds.verify_template_id.clear();
        ds.detected_template_id.clear();
        ds.consecutive_count = 0;
        
    } else if (elapsed_ms >= config_.verify_timeout_ms) {
        // タイムアウト: まだ検出されている → リトライまたは失敗
        result.retry_count = ds.verify_retry_count;
        
        if (ds.verify_retry_count < config_.verify_max_retry) {
            // リトライ
            result.should_retry = true;
            result.x = ds.verify_x;
            result.y = ds.verify_y;
            ds.verify_retry_count++;
            ds.verify_start = now;  // タイマーリセット
            
            MLOG_WARN("ai.vision", "検証失敗 → リトライ %d/%d: device=%s tpl=%s",
                       ds.verify_retry_count, config_.verify_max_retry,
                       device_id.c_str(), ds.verify_template_id.c_str());
        } else {
            // 最大リトライ超過 → 失敗として COOLDOWN へ
            // 検証記録: 失敗をカウント
            auto& vrec = verify_records_[ds.verify_template_id];
            vrec.fail_count++;
            vrec.retry_count += ds.verify_retry_count;
            vrec.last_fail = now;

            // 失敗率が高い場合は自動ignore
            if (vrec.should_ignore()) {
                if (!isIgnored(ds.verify_template_id)) {
                    ignoreTemplate(ds.verify_template_id);
                    MLOG_WARN("ai.vision", "自動ignore: tpl=%s success_rate=%.1f%%",
                        ds.verify_template_id.c_str(), vrec.success_rate() * 100.0f);
                }
            }

            result.timeout = true;
            
            VisionState old_state = ds.state;
            ds.cooldown_template_id = ds.verify_template_id;
            ds.cooldown_start = now;
            transitionTo(ds, VisionState::COOLDOWN);
            
            MLOG_WARN("ai.vision", "検証失敗 (タイムアウト) → COOLDOWN: device=%s tpl=%s retry=%d (success=%u fail=%u rate=%.1f%%)",
                       device_id.c_str(), ds.verify_template_id.c_str(), ds.verify_retry_count,
                       vrec.success_count, vrec.fail_count, vrec.success_rate() * 100.0f);
            
            mirage::StateChangeEvent evt;
            evt.device_id = device_id;
            evt.old_state = static_cast<int>(old_state);
            evt.new_state = static_cast<int>(VisionState::COOLDOWN);
            evt.template_id = ds.cooldown_template_id;
            evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            mirage::bus().publish(evt);
            
            ds.verify_template_id.clear();
            ds.detected_template_id.clear();
            ds.consecutive_count = 0;
        }
    }
    
    return result;
}

bool VisionDecisionEngine::isVerifying(const std::string& device_id) const {
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return false;
    return it->second.state == VisionState::VERIFYING;
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
        ds.layer2_last_call   = std::chrono::steady_clock::now();
        // Layer 0 無効時は IDLE から開始
        ds.state = config_.enable_layer0 ? VisionState::STANDBY : VisionState::IDLE;
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
        if (isIgnored(m.template_id)) continue;  // 無視リスト
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
// Layer 2: OllamaVision 非同期統合
// =============================================================================









// =============================================================================
// Layer 0: 操作検出ベースの待機モード
// =============================================================================

void VisionDecisionEngine::notifyUserInput(
    const std::string& device_id,
    std::chrono::steady_clock::time_point now)
{
    auto& ds = getOrCreateState(device_id);
    ds.last_user_input_time = now;

    // Layer 1/2 → Layer 0 (STANDBY) へ遷移
    if (ds.state != VisionState::STANDBY) {
        VisionState old_state = ds.state;
        transitionTo(ds, VisionState::STANDBY);

        // Layer 2 タスク実行中なら中断
        if (ds.layer2_task && ds.layer2_task->valid) {
            ds.layer2_task->valid = false;
            ds.layer2_task.reset();
        }

        MLOG_DEBUG("ai.vision", "ユーザー操作検出 → STANDBY (Layer 0): device=%s from=%s",
                   device_id.c_str(), visionStateToString(old_state));

        mirage::StateChangeEvent evt;
        evt.device_id = device_id;
        evt.old_state = static_cast<int>(old_state);
        evt.new_state = static_cast<int>(VisionState::STANDBY);
        evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        mirage::bus().publish(evt);

        // 状態リセット
        ds.detected_template_id.clear();
        ds.consecutive_count = 0;
        ds.consecutive_no_match = 0;
        ds.consecutive_same_match = 0;
    }
}

bool VisionDecisionEngine::checkLayer0Timeout(
    const std::string& device_id,
    std::chrono::steady_clock::time_point now)
{
    if (!config_.enable_layer0) return false;

    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return false;

    auto& ds = it->second;
    if (ds.state != VisionState::STANDBY) return false;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ds.last_user_input_time).count();

    if (elapsed >= config_.layer0_idle_timeout_ms) {
        VisionState old_state = ds.state;
        transitionTo(ds, VisionState::IDLE);
        ds.last_any_match_time = now;  // Layer 2トリガー用にリセット

        MLOG_INFO("ai.vision", "操作なし %lldms → IDLE (Layer 1): device=%s",
                  (long long)elapsed, device_id.c_str());

        mirage::StateChangeEvent evt;
        evt.device_id = device_id;
        evt.old_state = static_cast<int>(old_state);
        evt.new_state = static_cast<int>(VisionState::IDLE);
        evt.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        mirage::bus().publish(evt);

        return true;
    }

    return false;
}

bool VisionDecisionEngine::isStandby(const std::string& device_id) const {
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return true;  // 未登録 = STANDBY扱い
    return it->second.state == VisionState::STANDBY;
}



// =============================================================================
// テンプレート無視リスト
// =============================================================================

void VisionDecisionEngine::ignoreTemplate(const std::string& template_id) {
    ignored_templates_.insert(template_id);
    MLOG_INFO("ai.vision", "テンプレート無視追加: %s", template_id.c_str());
}

void VisionDecisionEngine::unignoreTemplate(const std::string& template_id) {
    ignored_templates_.erase(template_id);
    MLOG_INFO("ai.vision", "テンプレート無視解除: %s", template_id.c_str());
}

bool VisionDecisionEngine::isIgnored(const std::string& template_id) const {
    return ignored_templates_.count(template_id) > 0;
}

std::vector<std::string> VisionDecisionEngine::getIgnoredTemplates() const {
    return std::vector<std::string>(ignored_templates_.begin(), ignored_templates_.end());
}

void VisionDecisionEngine::clearIgnoredTemplates() {
    ignored_templates_.clear();
    MLOG_INFO("ai.vision", "テンプレート無視リストをクリア");
}


void VisionDecisionEngine::saveIgnoredTemplates(const std::string& path) const {
    std::ofstream ofs(path);
    if (!ofs) {
        MLOG_WARN("ai.vision", "無視リスト保存失敗: %s", path.c_str());
        return;
    }
    ofs << "{" << std::endl;
    ofs << "  \"ignored_templates\": [" << std::endl;
    bool first = true;
    for (const auto& tpl : ignored_templates_) {
        if (!first) ofs << "," << std::endl;
        ofs << "    \"" << tpl << "\"";
        first = false;
    }
    ofs << std::endl << "  ]" << std::endl;
    ofs << "}" << std::endl;
    MLOG_INFO("ai.vision", "無視リスト保存: %s (%d件)", path.c_str(), (int)ignored_templates_.size());
}

void VisionDecisionEngine::loadIgnoredTemplates(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        // ファイルなし = 初回起動、エラーではない
        return;
    }
    ignored_templates_.clear();
    std::string line;
    while (std::getline(ifs, line)) {
        // 簡易JSONパース: "template_name" を抽出
        size_t start = line.find('"');
        if (start == std::string::npos) continue;
        size_t end = line.find('"', start + 1);
        if (end == std::string::npos) continue;
        std::string tpl = line.substr(start + 1, end - start - 1);
        if (tpl != "ignored_templates") {
            ignored_templates_.insert(tpl);
        }
    }
    MLOG_INFO("ai.vision", "無視リスト読込: %s (%d件)", path.c_str(), (int)ignored_templates_.size());
}


void VisionDecisionEngine::setOllamaVision(std::shared_ptr<OllamaVision> ollama) {
    ollama_vision_ = std::move(ollama);
}

void VisionDecisionEngine::setLayer2Client(std::shared_ptr<Layer2Client> client) {
    layer2_client_ = std::move(client);
}

// --- Layer2: Layer2Client委譲実装 ---

bool VisionDecisionEngine::launchLayer2Async(
    const std::string& device_id, const uint8_t* rgba, int width, int height,
    std::chrono::steady_clock::time_point now)
{
    if (!layer2_client_) return false;
    if (!config_.enable_layer2) return false;

    return layer2_client_->launchAsync(device_id, rgba, width, height, now);
}

VisionDecisionEngine::Layer2Result VisionDecisionEngine::pollLayer2Result(
    const std::string& device_id)
{
    if (!layer2_client_) return {};
    auto r = layer2_client_->pollResult(device_id);
    if (!r.valid) return {};

    Layer2Result out;
    out.has_result   = true;
    out.found        = r.popup_detected;
    out.type         = r.popup_detected ? "popup" : "none";
    out.button_text  = "";
    out.x            = (r.click_x >= 0) ? r.click_x : 0;
    out.y            = (r.click_y >= 0) ? r.click_y : 0;
    out.elapsed_ms   = 0;
    out.error        = r.error;
    return out;
}

bool VisionDecisionEngine::isLayer2Running(const std::string& device_id) const {
    if (!layer2_client_) return false;
    return layer2_client_->isRunning(device_id);
}

bool VisionDecisionEngine::isLayer2OnCooldown(
    const std::string& device_id, std::chrono::steady_clock::time_point now) const
{
    if (!layer2_client_) return false;
    return layer2_client_->isOnCooldown(device_id, now);
}

void VisionDecisionEngine::cancelLayer2(const std::string& device_id) {
    if (layer2_client_) layer2_client_->cancel(device_id);
}

bool VisionDecisionEngine::shouldTriggerLayer2(
    const std::string& device_id, std::chrono::steady_clock::time_point now) const
{
    if (!layer2_client_) return false;
    if (!config_.enable_layer2) return false;
    if (layer2_client_->isRunning(device_id)) return false;
    if (layer2_client_->isOnCooldown(device_id, now)) return false;

    // デバイス状態チェック
    auto it = device_states_.find(device_id);
    if (it == device_states_.end()) return false;
    const auto& ds = it->second;

    // 連続マッチなしフレーム数でトリガー
    if (ds.consecutive_no_match >= config_.layer2_no_match_frames) return true;

    // 時間ベーストリガー
    if (config_.layer2_no_match_ms > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ds.last_any_match_time).count();
        if (elapsed >= config_.layer2_no_match_ms) return true;
    }

    return false;
}

bool VisionDecisionEngine::checkLayer2Freeze(
    const std::string&, std::chrono::steady_clock::time_point) { return false; }


// =============================================================================
// 検証記録（成功/失敗統計）
// =============================================================================

VerifyRecord VisionDecisionEngine::empty_verify_record_{};

const VerifyRecord& VisionDecisionEngine::getVerifyRecord(const std::string& template_id) const {
    auto it = verify_records_.find(template_id);
    if (it == verify_records_.end()) return empty_verify_record_;
    return it->second;
}

const std::unordered_map<std::string, VerifyRecord>& VisionDecisionEngine::getAllVerifyRecords() const {
    return verify_records_;
}

void VisionDecisionEngine::clearVerifyRecords() {
    verify_records_.clear();
    MLOG_INFO("ai.vision", "検証記録をクリア");
}

void VisionDecisionEngine::saveVerifyRecords(const std::string& path) const {
    std::string json = "{\n";
    bool first = true;
    for (const auto& [tpl_id, rec] : verify_records_) {
        if (!first) json += ",\n";
        first = false;
        json += "  \"" + tpl_id + "\": {\n";
        json += "    \"success_count\": " + std::to_string(rec.success_count) + ",\n";
        json += "    \"fail_count\": " + std::to_string(rec.fail_count) + ",\n";
        json += "    \"retry_count\": " + std::to_string(rec.retry_count) + "\n";
        json += "  }";
    }
    json += "\n}\n";

    std::ofstream ofs(path);
    if (ofs) {
        ofs << json;
        MLOG_INFO("ai.vision", "検証記録を保存: %s (%zu件)", path.c_str(), verify_records_.size());
    } else {
        MLOG_WARN("ai.vision", "検証記録の保存失敗: %s", path.c_str());
    }
}

void VisionDecisionEngine::loadVerifyRecords(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        MLOG_INFO("ai.vision", "検証記録ファイルなし: %s", path.c_str());
        return;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    verify_records_.clear();

    std::regex re(R"delim("([^"]+)":\s*\{\s*"success_count":\s*(\d+),\s*"fail_count":\s*(\d+),\s*"retry_count":\s*(\d+)\s*\})delim");
    std::sregex_iterator it(content.begin(), content.end(), re);
    std::sregex_iterator end_it;

    while (it != end_it) {
        std::smatch m = *it;
        VerifyRecord rec;
        rec.success_count = static_cast<uint32_t>(std::stoul(m[2].str()));
        rec.fail_count = static_cast<uint32_t>(std::stoul(m[3].str()));
        rec.retry_count = static_cast<uint32_t>(std::stoul(m[4].str()));
        verify_records_[m[1].str()] = rec;
        ++it;
    }

    MLOG_INFO("ai.vision", "検証記録を読込: %s (%zu件)", path.c_str(), verify_records_.size());
}

void VisionDecisionEngine::autoIgnoreFailingTemplates(float threshold, uint32_t min_samples) {
    int ignored_count = 0;
    for (const auto& [tpl_id, rec] : verify_records_) {
        if (rec.should_ignore(threshold, min_samples)) {
            if (!isIgnored(tpl_id)) {
                ignoreTemplate(tpl_id);
                ignored_count++;
                MLOG_WARN("ai.vision", "自動ignore: tpl=%s success_rate=%.1f%% (threshold=%.1f%% samples=%u)",
                    tpl_id.c_str(), rec.success_rate() * 100.0f, threshold * 100.0f,
                    rec.success_count + rec.fail_count);
            }
        }
    }
    if (ignored_count > 0) {
        MLOG_INFO("ai.vision", "自動ignoreテンプレート: %d件", ignored_count);
    }
}

} // namespace mirage::ai
