// =============================================================================
// MirageSystem v2 GUI - AI Engine Control Panel
// =============================================================================
// AIエンジンの制御・監視パネル。
// テンプレートマッチング状態、VisionDecisionEngine、LearningMode制御。
// =============================================================================
#pragma once

namespace mirage::gui::ai_panel {

// AI制御パネル描画（メインループから毎フレーム呼び出し）
void renderAIPanel();

// 初期化（EventBus購読等）
void init();

// シャットダウン（購読解除）
void shutdown();

} // namespace mirage::gui::ai_panel
