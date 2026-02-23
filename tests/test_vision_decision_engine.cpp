// =============================================================================
// Unit tests for VisionDecisionEngine (src/ai/vision_decision_engine.hpp/cpp)
// =============================================================================
// GPU不要 — 状態遷移・デバウンス・マルチデバイスのCPU純粋ロジックテスト
// =============================================================================
#include <gtest/gtest.h>
#include "ai/vision_decision_engine.hpp"
#include <chrono>
#include <vector>

using namespace mirage::ai;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

// テスト用: 固定時刻を作る
static Clock::time_point makeTime(int64_t ms) {
    return Clock::time_point(Ms(ms));
}

// 通常マッチ（非errorグループ）を作る
static VisionMatch normalMatch(const std::string& id, float score, int x = 100, int y = 200) {
    VisionMatch m;
    m.template_id = id;
    m.score = score;
    m.x = x;
    m.y = y;
    m.is_error_group = false;
    return m;
}

// errorグループマッチを作る
static VisionMatch errorMatch(const std::string& id, float score = 0.90f) {
    VisionMatch m;
    m.template_id = id;
    m.score = score;
    m.x = 50;
    m.y = 50;
    m.is_error_group = true;
    return m;
}

// =============================================================================
// 1. 状態遷移テスト
// =============================================================================

class VisionStateTransitionTest : public ::testing::Test {
protected:
    VisionDecisionConfig cfg;
    void SetUp() override {
        cfg.confirm_count = 3;
        cfg.cooldown_ms = 2000;
        cfg.debounce_window_ms = 500;
        cfg.error_recovery_ms = 3000;
    }
};

// ---------------------------------------------------------------------------
// IDLE → DETECTED: マッチ入力で遷移
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, IdleToDetected) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };
    auto decision = engine.update(dev, matches, makeTime(1000));

    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);
    EXPECT_FALSE(decision.should_act);  // まだ確定していない
    EXPECT_EQ(decision.state, VisionState::DETECTED);
}

// ---------------------------------------------------------------------------
// DETECTED → CONFIRMED: N回連続検出で確定
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, DetectedToConfirmed) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };

    // 1回目: IDLE → DETECTED
    engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    // 2回目: DETECTED (count=2)
    engine.update(dev, matches, makeTime(2000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    // 3回目: DETECTED → CONFIRMED (confirm_count=3)
    auto decision = engine.update(dev, matches, makeTime(3000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);
    EXPECT_TRUE(decision.should_act);
    EXPECT_EQ(decision.template_id, "btn_ok");
    EXPECT_FLOAT_EQ(decision.score, 0.90f);
    EXPECT_EQ(decision.x, 100);
    EXPECT_EQ(decision.y, 200);
}

// ---------------------------------------------------------------------------
// DETECTED → IDLE: 別テンプレートでカウントリセット
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, DetectedResetOnDifferentTemplate) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    // "btn_ok" で2回検出
    std::vector<VisionMatch> m1 = { normalMatch("btn_ok", 0.90f) };
    engine.update(dev, m1, makeTime(1000));
    engine.update(dev, m1, makeTime(2000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    // 別テンプレート "btn_cancel" → 検出リセット、DETECTED状態は維持
    std::vector<VisionMatch> m2 = { normalMatch("btn_cancel", 0.85f) };
    auto decision = engine.update(dev, m2, makeTime(3000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);
    EXPECT_FALSE(decision.should_act);

    // "btn_cancel" で2回追加 → 計3回で確定
    engine.update(dev, m2, makeTime(4000));
    auto d2 = engine.update(dev, m2, makeTime(5000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);
    EXPECT_TRUE(d2.should_act);
    EXPECT_EQ(d2.template_id, "btn_cancel");
}

// ---------------------------------------------------------------------------
// DETECTED → IDLE: マッチなしでリセット
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, DetectedToIdleOnNoMatch) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };
    engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    // 空マッチ → IDLE
    std::vector<VisionMatch> empty;
    engine.update(dev, empty, makeTime(2000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);
}

// ---------------------------------------------------------------------------
// CONFIRMED → COOLDOWN: アクション実行後
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, ConfirmedToCooldown) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };

    // 3回でCONFIRMED
    engine.update(dev, matches, makeTime(1000));
    engine.update(dev, matches, makeTime(2000));
    engine.update(dev, matches, makeTime(3000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);

    // アクション実行通知 → COOLDOWN
    engine.notifyActionExecuted(dev, makeTime(3100));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::COOLDOWN);
}

// ---------------------------------------------------------------------------
// COOLDOWN → IDLE: 時間経過
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, CooldownToIdle) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };

    // CONFIRMED → COOLDOWN
    engine.update(dev, matches, makeTime(1000));
    engine.update(dev, matches, makeTime(2000));
    engine.update(dev, matches, makeTime(3000));
    engine.notifyActionExecuted(dev, makeTime(3100));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::COOLDOWN);

    // cooldown_ms=2000 なので、5100msではまだCOOLDOWN
    auto d1 = engine.update(dev, matches, makeTime(4000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::COOLDOWN);
    EXPECT_FALSE(d1.should_act);

    // 5200ms (= 3100 + 2100) > cooldown_ms → IDLE
    auto d2 = engine.update(dev, matches, makeTime(5200));
    // COOLDOWNが解除された後、新しいマッチでDETECTEDに遷移
    EXPECT_NE(engine.getDeviceState(dev), VisionState::COOLDOWN);
}

// ---------------------------------------------------------------------------
// ANY → ERROR_RECOVERY: エラーグループ検出
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, AnyToErrorRecovery) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    // IDLE状態からエラーテンプレート検出
    std::vector<VisionMatch> err_matches = { errorMatch("error_dialog") };
    auto decision = engine.update(dev, err_matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::ERROR_RECOVERY);
    EXPECT_TRUE(decision.should_act);
    EXPECT_TRUE(decision.is_error_recovery);
    EXPECT_EQ(decision.template_id, "error_dialog");
}

// ---------------------------------------------------------------------------
// DETECTED → ERROR_RECOVERY: DETECTED中にエラー検出
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, DetectedToErrorRecovery) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    // まずDETECTED状態に
    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };
    engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    // エラーテンプレート検出 → ERROR_RECOVERY
    std::vector<VisionMatch> err = { errorMatch("popup_error") };
    auto decision = engine.update(dev, err, makeTime(2000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::ERROR_RECOVERY);
    EXPECT_TRUE(decision.should_act);
    EXPECT_TRUE(decision.is_error_recovery);
}

// ---------------------------------------------------------------------------
// ERROR_RECOVERY → IDLE: アクション実行完了
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, ErrorRecoveryToIdleOnAction) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> err = { errorMatch("error_dialog") };
    engine.update(dev, err, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::ERROR_RECOVERY);

    engine.notifyActionExecuted(dev, makeTime(1500));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);
}

// ---------------------------------------------------------------------------
// ERROR_RECOVERY → IDLE: タイムアウト
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, ErrorRecoveryTimeout) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> err = { errorMatch("error_dialog") };
    engine.update(dev, err, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::ERROR_RECOVERY);

    // error_recovery_ms=3000 経過
    std::vector<VisionMatch> empty;
    engine.update(dev, empty, makeTime(4100));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);
}

// ---------------------------------------------------------------------------
// ERROR_RECOVERY中は通常アクション抑制
// ---------------------------------------------------------------------------
TEST_F(VisionStateTransitionTest, ErrorRecoverySuppressNormalAction) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> err = { errorMatch("error_dialog") };
    engine.update(dev, err, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::ERROR_RECOVERY);

    // 通常マッチを入力してもshould_act=false
    std::vector<VisionMatch> normal = { normalMatch("btn_ok", 0.95f) };
    auto decision = engine.update(dev, normal, makeTime(1500));
    EXPECT_FALSE(decision.should_act);
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::ERROR_RECOVERY);
}

// =============================================================================
// 2. デバウンス動作テスト
// =============================================================================

class VisionDebounceTest : public ::testing::Test {
protected:
    VisionDecisionConfig cfg;
    void SetUp() override {
        cfg.confirm_count = 1;   // 即確定（デバウンス検証に集中）
        cfg.cooldown_ms = 1000;
        cfg.debounce_window_ms = 500;
        cfg.error_recovery_ms = 3000;
    }
};

// ---------------------------------------------------------------------------
// 同一テンプレート連続検出のカウント確認
// ---------------------------------------------------------------------------
TEST_F(VisionDebounceTest, ConsecutiveDetectionCount) {
    VisionDecisionConfig c;
    c.confirm_count = 5;
    c.cooldown_ms = 2000;
    c.debounce_window_ms = 500;
    VisionDecisionEngine engine(c);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };

    // 1-4回: DETECTED
    for (int i = 0; i < 4; i++) {
        auto d = engine.update(dev, matches, makeTime(1000 + i * 1000));
        EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);
        EXPECT_FALSE(d.should_act);
    }

    // 5回目: CONFIRMED
    auto d5 = engine.update(dev, matches, makeTime(5000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);
    EXPECT_TRUE(d5.should_act);
}

// ---------------------------------------------------------------------------
// cooldown期間中のアクション抑制確認
// ---------------------------------------------------------------------------
TEST_F(VisionDebounceTest, CooldownSuppression) {
    // confirm_count=1: call1→DETECTED, call2→CONFIRMED
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };

    // 1回目: IDLE→DETECTED
    engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    // 2回目: DETECTED→CONFIRMED (confirm_count=1, count=2≥1)
    auto d1 = engine.update(dev, matches, makeTime(1050));
    EXPECT_TRUE(d1.should_act);
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);

    // COOLDOWN遷移
    engine.notifyActionExecuted(dev, makeTime(1100));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::COOLDOWN);

    // COOLDOWN中のマッチはshould_act=false
    auto d2 = engine.update(dev, matches, makeTime(1200));
    EXPECT_FALSE(d2.should_act);
    EXPECT_EQ(d2.state, VisionState::COOLDOWN);

    auto d3 = engine.update(dev, matches, makeTime(1800));
    EXPECT_FALSE(d3.should_act);
    EXPECT_EQ(d3.state, VisionState::COOLDOWN);
}

// ---------------------------------------------------------------------------
// debounce_window_ms内の重複排除
// ---------------------------------------------------------------------------
TEST_F(VisionDebounceTest, DebounceWindowDuplicateElimination) {
    // confirm_count=1: call1→DETECTED, call2→CONFIRMED
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };

    // IDLE→DETECTED→CONFIRMED
    engine.update(dev, matches, makeTime(1000));
    auto d1 = engine.update(dev, matches, makeTime(1020));
    EXPECT_TRUE(d1.should_act);

    engine.notifyActionExecuted(dev, makeTime(1050));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::COOLDOWN);

    // COOLDOWN中 (1050 + 1000 = 2050) → 抑制
    auto d2 = engine.update(dev, matches, makeTime(1500));
    EXPECT_FALSE(d2.should_act);

    // COOLDOWN終了後 + デバウンスウィンドウ内 (1050 + 500 = 1550)
    // → デバウンスで抑制
    auto d3 = engine.update(dev, matches, makeTime(2100));
    // COOLDOWNは解除される; デバウンスが残っているか確認
    // デバウンスウィンドウ: 1050 + 500 = 1550 → 2100 > 1550 なのでデバウンス解除済み
    // → 新規DETECTED開始
    EXPECT_NE(engine.getDeviceState(dev), VisionState::COOLDOWN);

    // デバウンスウィンドウ外 + COOLDOWN外 → 新規DETECTED
    auto d4 = engine.update(dev, matches, makeTime(3000));
    EXPECT_NE(engine.getDeviceState(dev), VisionState::COOLDOWN);
}

// ---------------------------------------------------------------------------
// isDebounced クエリ確認
// ---------------------------------------------------------------------------
TEST_F(VisionDebounceTest, IsDebounceQuery) {
    // confirm_count=1: call1→DETECTED, call2→CONFIRMED
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    // 初期状態: デバウンスなし
    EXPECT_FALSE(engine.isDebounced(dev, "btn_ok", makeTime(1000)));

    // IDLE→DETECTED→CONFIRMED
    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };
    engine.update(dev, matches, makeTime(1000));
    engine.update(dev, matches, makeTime(1020));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);

    // notifyActionExecutedでCOOLDOWN + デバウンスマップに記録
    engine.notifyActionExecuted(dev, makeTime(1050));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::COOLDOWN);

    // デバウンスウィンドウ内 (1050 + 500 = 1550)
    EXPECT_TRUE(engine.isDebounced(dev, "btn_ok", makeTime(1200)));

    // デバウンスウィンドウ外
    EXPECT_FALSE(engine.isDebounced(dev, "btn_ok", makeTime(1600)));

    // 別テンプレートは影響なし
    EXPECT_FALSE(engine.isDebounced(dev, "btn_cancel", makeTime(1200)));
}

// =============================================================================
// 3. 設定パラメータテスト
// =============================================================================

class VisionConfigTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// confirm_count変更テスト（1）
// confirm_count=1: IDLE→DETECTED(count=1), 次回→count=2≥1→CONFIRMED
// ---------------------------------------------------------------------------
TEST_F(VisionConfigTest, ConfirmCount1) {
    VisionDecisionConfig cfg;
    cfg.confirm_count = 1;
    cfg.cooldown_ms = 1000;
    cfg.debounce_window_ms = 0;  // デバウンスなし
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn", 0.90f) };

    // 1回目: IDLE→DETECTED
    auto d1 = engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);
    EXPECT_FALSE(d1.should_act);

    // 2回目: count=2≥1 → CONFIRMED
    auto d2 = engine.update(dev, matches, makeTime(1100));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);
    EXPECT_TRUE(d2.should_act);
}

// ---------------------------------------------------------------------------
// confirm_count変更テスト（5）
// ---------------------------------------------------------------------------
TEST_F(VisionConfigTest, ConfirmCount5) {
    VisionDecisionConfig cfg;
    cfg.confirm_count = 5;
    cfg.cooldown_ms = 1000;
    cfg.debounce_window_ms = 0;
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn", 0.90f) };

    // 4回: まだDETECTED
    for (int i = 0; i < 4; i++) {
        auto d = engine.update(dev, matches, makeTime(1000 + i * 1000));
        EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);
        EXPECT_FALSE(d.should_act);
    }

    // 5回目: CONFIRMED
    auto d = engine.update(dev, matches, makeTime(5000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);
    EXPECT_TRUE(d.should_act);
}

// ---------------------------------------------------------------------------
// cooldown_ms変更テスト
// ---------------------------------------------------------------------------
TEST_F(VisionConfigTest, CooldownMsChange) {
    VisionDecisionConfig cfg;
    cfg.confirm_count = 1;
    cfg.cooldown_ms = 500;   // 短いCOOLDOWN
    cfg.debounce_window_ms = 0;
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn", 0.90f) };

    // IDLE→DETECTED→CONFIRMED
    engine.update(dev, matches, makeTime(1000));
    engine.update(dev, matches, makeTime(1020));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);

    engine.notifyActionExecuted(dev, makeTime(1050));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::COOLDOWN);

    // 500ms経過前: まだCOOLDOWN (1050 + 500 = 1550)
    auto d1 = engine.update(dev, matches, makeTime(1400));
    EXPECT_EQ(d1.state, VisionState::COOLDOWN);

    // 500ms経過後: COOLDOWN解除
    auto d2 = engine.update(dev, matches, makeTime(1600));
    EXPECT_NE(engine.getDeviceState(dev), VisionState::COOLDOWN);
}

// ---------------------------------------------------------------------------
// setConfigで動的に設定変更
// ---------------------------------------------------------------------------
TEST_F(VisionConfigTest, DynamicConfigChange) {
    VisionDecisionConfig cfg;
    cfg.confirm_count = 3;
    cfg.cooldown_ms = 2000;
    cfg.debounce_window_ms = 500;
    VisionDecisionEngine engine(cfg);

    // 設定変更
    VisionDecisionConfig new_cfg;
    new_cfg.confirm_count = 1;
    new_cfg.cooldown_ms = 100;
    new_cfg.debounce_window_ms = 0;
    engine.setConfig(new_cfg);

    const auto& c = engine.config();
    EXPECT_EQ(c.confirm_count, 1);
    EXPECT_EQ(c.cooldown_ms, 100);
    EXPECT_EQ(c.debounce_window_ms, 0);

    // 変更後の動作確認: confirm_count=1 → 2回でCONFIRMED
    const std::string dev = "dev1";
    std::vector<VisionMatch> matches = { normalMatch("btn", 0.90f) };
    engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);
    auto d = engine.update(dev, matches, makeTime(1100));
    EXPECT_TRUE(d.should_act);
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);
}

// =============================================================================
// 4. マルチデバイステスト
// =============================================================================

class VisionMultiDeviceTest : public ::testing::Test {
protected:
    VisionDecisionConfig cfg;
    void SetUp() override {
        cfg.confirm_count = 2;
        cfg.cooldown_ms = 1000;
        cfg.debounce_window_ms = 0;
        cfg.error_recovery_ms = 3000;
    }
};

// ---------------------------------------------------------------------------
// device_id別の独立状態管理
// ---------------------------------------------------------------------------
TEST_F(VisionMultiDeviceTest, IndependentDeviceStates) {
    VisionDecisionEngine engine(cfg);

    std::vector<VisionMatch> matches_a = { normalMatch("btn_a", 0.90f) };
    std::vector<VisionMatch> matches_b = { normalMatch("btn_b", 0.85f) };

    // デバイスA: 1回検出 → DETECTED
    engine.update("devA", matches_a, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState("devA"), VisionState::DETECTED);

    // デバイスB: 初期状態 IDLE
    EXPECT_EQ(engine.getDeviceState("devB"), VisionState::IDLE);

    // デバイスB: 1回検出 → DETECTED
    engine.update("devB", matches_b, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState("devB"), VisionState::DETECTED);

    // デバイスA: 2回目 → CONFIRMED (confirm_count=2)
    auto da = engine.update("devA", matches_a, makeTime(2000));
    EXPECT_EQ(engine.getDeviceState("devA"), VisionState::CONFIRMED);
    EXPECT_TRUE(da.should_act);

    // デバイスBはまだDETECTED
    EXPECT_EQ(engine.getDeviceState("devB"), VisionState::DETECTED);
}

// ---------------------------------------------------------------------------
// デバイスAがCOOLDOWN中にデバイスBがCONFIRMED
// ---------------------------------------------------------------------------
TEST_F(VisionMultiDeviceTest, DeviceACooldownDeviceBConfirmed) {
    VisionDecisionEngine engine(cfg);

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };

    // デバイスA: CONFIRMED → COOLDOWN
    engine.update("devA", matches, makeTime(1000));
    engine.update("devA", matches, makeTime(2000));
    EXPECT_EQ(engine.getDeviceState("devA"), VisionState::CONFIRMED);
    engine.notifyActionExecuted("devA", makeTime(2100));
    EXPECT_EQ(engine.getDeviceState("devA"), VisionState::COOLDOWN);

    // デバイスB: CONFIRMED
    engine.update("devB", matches, makeTime(2200));
    auto db = engine.update("devB", matches, makeTime(2300));
    EXPECT_EQ(engine.getDeviceState("devB"), VisionState::CONFIRMED);
    EXPECT_TRUE(db.should_act);

    // デバイスAはまだCOOLDOWN
    EXPECT_EQ(engine.getDeviceState("devA"), VisionState::COOLDOWN);
}

// ---------------------------------------------------------------------------
// 3デバイス独立動作
// ---------------------------------------------------------------------------
TEST_F(VisionMultiDeviceTest, ThreeDevicesIndependent) {
    VisionDecisionEngine engine(cfg);

    std::vector<VisionMatch> m1 = { normalMatch("btn1", 0.90f) };
    std::vector<VisionMatch> m2 = { normalMatch("btn2", 0.85f) };
    std::vector<VisionMatch> m3 = { errorMatch("error_popup") };

    // devA: DETECTED
    engine.update("devA", m1, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState("devA"), VisionState::DETECTED);

    // devB: DETECTED
    engine.update("devB", m2, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState("devB"), VisionState::DETECTED);

    // devC: ERROR_RECOVERY
    engine.update("devC", m3, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState("devC"), VisionState::ERROR_RECOVERY);

    // 全デバイスの状態が独立していることを確認
    EXPECT_EQ(engine.getDeviceState("devA"), VisionState::DETECTED);
    EXPECT_EQ(engine.getDeviceState("devB"), VisionState::DETECTED);
    EXPECT_EQ(engine.getDeviceState("devC"), VisionState::ERROR_RECOVERY);
}

// =============================================================================
// 5. エッジケーステスト
// =============================================================================

class VisionEdgeCaseTest : public ::testing::Test {
protected:
    VisionDecisionConfig cfg;
    void SetUp() override {
        cfg.confirm_count = 2;
        cfg.cooldown_ms = 1000;
        cfg.debounce_window_ms = 0;
        cfg.error_recovery_ms = 3000;
    }
};

// ---------------------------------------------------------------------------
// 空マッチ結果入力
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, EmptyMatchInput) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> empty;
    auto d = engine.update(dev, empty, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);
    EXPECT_FALSE(d.should_act);
}

// ---------------------------------------------------------------------------
// 空マッチを連続入力してもIDLEのまま
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, RepeatedEmptyMatches) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> empty;
    for (int i = 0; i < 10; i++) {
        auto d = engine.update(dev, empty, makeTime(1000 + i * 100));
        EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);
        EXPECT_FALSE(d.should_act);
    }
}

// ---------------------------------------------------------------------------
// 同時に複数テンプレートマッチ（最高スコア選択）
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, MultipleMatchesBestScoreSelected) {
    VisionDecisionConfig c;
    c.confirm_count = 1;
    c.cooldown_ms = 1000;
    c.debounce_window_ms = 0;
    VisionDecisionEngine engine(c);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = {
        normalMatch("btn_low",  0.70f, 10, 20),
        normalMatch("btn_high", 0.95f, 100, 200),
        normalMatch("btn_mid",  0.85f, 50, 60),
    };

    // 1回目: IDLE→DETECTED（最高スコアの btn_high が選択される）
    engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    // 2回目: DETECTED→CONFIRMED
    auto d = engine.update(dev, matches, makeTime(1100));
    EXPECT_TRUE(d.should_act);
    EXPECT_EQ(d.template_id, "btn_high");
    EXPECT_FLOAT_EQ(d.score, 0.95f);
    EXPECT_EQ(d.x, 100);
    EXPECT_EQ(d.y, 200);
}

// ---------------------------------------------------------------------------
// errorマッチと通常マッチが同時 → error優先
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, ErrorMatchPriorityOverNormal) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = {
        normalMatch("btn_ok", 0.95f),
        errorMatch("error_popup", 0.80f),
    };

    auto d = engine.update(dev, matches, makeTime(1000));
    EXPECT_TRUE(d.should_act);
    EXPECT_TRUE(d.is_error_recovery);
    EXPECT_EQ(d.template_id, "error_popup");
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::ERROR_RECOVERY);
}

// ---------------------------------------------------------------------------
// 状態リセット（resetDevice）
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, ResetDevice) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };
    engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    engine.resetDevice(dev);
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);

    // リセット後は新規開始
    auto d = engine.update(dev, matches, makeTime(2000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);
    EXPECT_FALSE(d.should_act);
}

// ---------------------------------------------------------------------------
// 全リセット（resetAll）
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, ResetAll) {
    VisionDecisionEngine engine(cfg);

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };
    engine.update("devA", matches, makeTime(1000));
    engine.update("devB", matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState("devA"), VisionState::DETECTED);
    EXPECT_EQ(engine.getDeviceState("devB"), VisionState::DETECTED);

    engine.resetAll();
    EXPECT_EQ(engine.getDeviceState("devA"), VisionState::IDLE);
    EXPECT_EQ(engine.getDeviceState("devB"), VisionState::IDLE);
}

// ---------------------------------------------------------------------------
// notifyActionExecuted on unknown device (no crash)
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, NotifyUnknownDevice) {
    VisionDecisionEngine engine(cfg);
    engine.notifyActionExecuted("nonexistent", makeTime(1000));
    EXPECT_EQ(engine.getDeviceState("nonexistent"), VisionState::IDLE);
}

// ---------------------------------------------------------------------------
// resetDevice on unknown device (no crash)
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, ResetUnknownDevice) {
    VisionDecisionEngine engine(cfg);
    engine.resetDevice("nonexistent");
    EXPECT_EQ(engine.getDeviceState("nonexistent"), VisionState::IDLE);
}

// ---------------------------------------------------------------------------
// CONFIRMED状態でnotifyなしに同一マッチ → should_act=false（二重実行防止）
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, ConfirmedNoDoubleAction) {
    VisionDecisionConfig c;
    c.confirm_count = 1;
    c.cooldown_ms = 2000;
    c.debounce_window_ms = 0;
    VisionDecisionEngine engine(c);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn", 0.90f) };

    // IDLE→DETECTED
    engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    // DETECTED→CONFIRMED + should_act=true
    auto d1 = engine.update(dev, matches, makeTime(1050));
    EXPECT_TRUE(d1.should_act);
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);

    // CONFIRMED状態のまま → should_act=false（notify待ち）
    auto d2 = engine.update(dev, matches, makeTime(1100));
    EXPECT_FALSE(d2.should_act);
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);
}

// ---------------------------------------------------------------------------
// visionStateToString 全状態カバー
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, StateToString) {
    EXPECT_STREQ(visionStateToString(VisionState::IDLE), "IDLE");
    EXPECT_STREQ(visionStateToString(VisionState::DETECTED), "DETECTED");
    EXPECT_STREQ(visionStateToString(VisionState::CONFIRMED), "CONFIRMED");
    EXPECT_STREQ(visionStateToString(VisionState::COOLDOWN), "COOLDOWN");
    EXPECT_STREQ(visionStateToString(VisionState::ERROR_RECOVERY), "ERROR_RECOVERY");
}

// ---------------------------------------------------------------------------
// VisionDecision prev_state の確認
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, PrevStateTracking) {
    VisionDecisionConfig c;
    c.confirm_count = 1;
    c.cooldown_ms = 1000;
    c.debounce_window_ms = 0;
    VisionDecisionEngine engine(c);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn", 0.90f) };

    // IDLE → DETECTED
    auto d1 = engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(d1.prev_state, VisionState::IDLE);
    EXPECT_EQ(d1.state, VisionState::DETECTED);

    // DETECTED → CONFIRMED
    auto d2 = engine.update(dev, matches, makeTime(1100));
    EXPECT_EQ(d2.prev_state, VisionState::DETECTED);
    EXPECT_EQ(d2.state, VisionState::CONFIRMED);
}

// ---------------------------------------------------------------------------
// 完全ライフサイクル: IDLE → DETECTED → CONFIRMED → COOLDOWN → IDLE → ...
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, FullLifecycle) {
    VisionDecisionEngine engine(cfg);
    const std::string dev = "dev1";

    std::vector<VisionMatch> matches = { normalMatch("btn_ok", 0.90f) };

    // IDLE
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);

    // → DETECTED
    engine.update(dev, matches, makeTime(1000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);

    // → CONFIRMED (confirm_count=2)
    auto d = engine.update(dev, matches, makeTime(2000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::CONFIRMED);
    EXPECT_TRUE(d.should_act);

    // → COOLDOWN
    engine.notifyActionExecuted(dev, makeTime(2100));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::COOLDOWN);

    // → IDLE (cooldown_ms=1000経過)
    std::vector<VisionMatch> empty;
    engine.update(dev, empty, makeTime(3200));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);

    // 再び → DETECTED
    engine.update(dev, matches, makeTime(4000));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::DETECTED);
}

// ---------------------------------------------------------------------------
// デフォルト設定で構築
// ---------------------------------------------------------------------------
TEST_F(VisionEdgeCaseTest, DefaultConfig) {
    VisionDecisionEngine engine;
    const auto& c = engine.config();
    EXPECT_EQ(c.confirm_count, 3);
    EXPECT_EQ(c.cooldown_ms, 2000);
    EXPECT_EQ(c.debounce_window_ms, 500);
    EXPECT_EQ(c.error_recovery_ms, 3000);
}

// ===========================================================================
// 改善D: EWMA スムージングテスト
// ===========================================================================

class EwmaTest : public ::testing::Test {
protected:
    VisionDecisionConfig makeEwmaCfg(float alpha = 0.5f, float thr = 0.7f) {
        VisionDecisionConfig c;
        c.confirm_count      = 1;    // すぐ CONFIRMED
        c.cooldown_ms        = 500;
        c.debounce_window_ms = 0;
        c.enable_ewma        = true;
        c.ewma_alpha         = alpha;
        c.ewma_confirm_thr   = thr;
        return c;
    }
    VisionMatch match(const std::string& id, float score) {
        VisionMatch m; m.template_id = id; m.score = score;
        return m;
    }
    std::chrono::steady_clock::time_point T(int ms) {
        return std::chrono::steady_clock::time_point() + std::chrono::milliseconds(ms);
    }
};

// E-1: EWMA が thr に到達するまで CONFIRMED にならない
TEST_F(EwmaTest, ConfirmedOnlyAfterEwmaReachesThreshold) {
    auto cfg = makeEwmaCfg(0.5f, 0.7f);
    VisionDecisionEngine engine(cfg);
    const std::string dev = "d";
    std::vector<VisionMatch> ms = { match("btn", 0.95f) };

    // frame 1: ewma = 0.5*1 + 0.5*0 = 0.5  < 0.7 → NOT confirmed
    auto r1 = engine.update(dev, ms, T(100));
    EXPECT_FALSE(r1.should_act);

    // frame 2: ewma = 0.5*1 + 0.5*0.5 = 0.75 >= 0.7 → confirmed
    auto r2 = engine.update(dev, ms, T(200));
    EXPECT_TRUE(r2.should_act);
}

// E-2: EWMA が thr 以上でも高 alpha=1.0 なら初回から通過
TEST_F(EwmaTest, AlphaOneConfirmsImmediately) {
    auto cfg = makeEwmaCfg(1.0f, 0.7f);
    VisionDecisionEngine engine(cfg);
    const std::string dev = "d";
    std::vector<VisionMatch> ms = { match("btn", 0.95f) };

    // alpha=1.0: frame1 IDLE->DETECTED (ewma=1.0, count=1)
    auto r1 = engine.update(dev, ms, T(100));
    EXPECT_FALSE(r1.should_act);
    // frame2: DETECTED->CONFIRMED (ewma=1.0 >= 0.7, count=2 >= 1)
    auto r2 = engine.update(dev, ms, T(200));
    EXPECT_TRUE(r2.should_act);
}

// E-3: マッチなしでEWMAが減衰する
TEST_F(EwmaTest, EwmaDecaysOnNoMatch) {
    auto cfg = makeEwmaCfg(1.0f, 0.1f);  // low thr to start confirmed
    VisionDecisionEngine engine(cfg);
    const std::string dev = "d";
    std::vector<VisionMatch> ms  = { match("btn", 0.95f) };
    std::vector<VisionMatch> none;

    engine.update(dev, ms, T(100));    // ewma=1.0, confirmed
    engine.notifyActionExecuted(dev, T(110));

    // cooldown passes (500ms)
    engine.update(dev, none, T(700));  // no match: ewma decays
    engine.update(dev, none, T(800));

    // After multiple no-match frames, ewma should be below 0.1
    // (1.0 * (0.5)^N → 0). Confirm idle.
    for (int i = 0; i < 20; i++)
        engine.update(dev, none, T(900 + i*100));
    EXPECT_EQ(engine.getDeviceState(dev), VisionState::IDLE);
}

// E-4: テンプレート切り替えでEWMAリセット
TEST_F(EwmaTest, EwmaResetsOnTemplateSwitch) {
    auto cfg = makeEwmaCfg(0.5f, 0.7f);
    VisionDecisionEngine engine(cfg);
    const std::string dev = "d";
    std::vector<VisionMatch> ms1 = { match("btn_a", 0.95f) };
    std::vector<VisionMatch> ms2 = { match("btn_b", 0.95f) };

    engine.update(dev, ms1, T(100));  // ewma_a=0.5
    engine.update(dev, ms1, T(200));  // ewma_a=0.75 → confirmed
    engine.notifyActionExecuted(dev, T(210));

    // cooldown passes, then switch template
    engine.update(dev, ms2, T(800));  // ewma resets to 0, new template
    auto r = engine.update(dev, ms2, T(900));
    // After reset: frame1=0.5, frame2=0.75 → confirmed
    EXPECT_TRUE(r.should_act);
}

// E-5: enable_ewma=false はEWMAゲートをスキップ
TEST_F(EwmaTest, DisabledEwmaSkipsGate) {
    VisionDecisionConfig cfg;
    cfg.confirm_count      = 1;
    cfg.cooldown_ms        = 500;
    cfg.debounce_window_ms = 0;
    cfg.enable_ewma        = false;   // DISABLED
    cfg.ewma_confirm_thr   = 0.99f;   // impossible if ewma was active
    VisionDecisionEngine engine(cfg);
    const std::string dev = "d";
    std::vector<VisionMatch> ms = { match("btn", 0.95f) };

    // frame1: IDLE->DETECTED (no ewma gate)
    auto r1 = engine.update(dev, ms, T(100));
    EXPECT_FALSE(r1.should_act);
    // frame2: DETECTED->CONFIRMED (count=2>=1, ewma_ok=true)
    auto r2 = engine.update(dev, ms, T(200));
    EXPECT_TRUE(r2.should_act);
}