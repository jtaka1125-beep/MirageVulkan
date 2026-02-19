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
            decision.state = VisionState::COOLDOWN;
            return decision;
        }
    }

    // === ベストマッチ選択 ===
    const VisionMatch* best = findBestMatch(matches);

    if (!best) {
        // マッチなし
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

    // === デバウンスチェック ===
    if (isDebounced(device_id, best->template_id, now)) {
        decision.state = ds.state;
        return decision;
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

                if (ds.consecutive_count >= config_.confirm_count) {
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
    return device_states_[device_id];
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

} // namespace mirage::ai
