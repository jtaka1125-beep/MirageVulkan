# AI Pipeline Status
> Updated: 2026-03-10 | src/ai/ および Android ai/ を直接確認済み

---

## AIパイプライン全体像

```
[Android (com.mirage.capture)]         [PC / MirageVulkan]
                                        
AiStream.kt                             AIEngine (C++)
  MediaProjection                         Layer 1: VulkanTemplateMatcher
  └→ VirtualDisplay(RGBA)                   NCC テンプレートマッチ (GPU)
  └→ ImageReader(RGBA_8888)               Layer 2: OCR (Tesseract)
  └→ AiFrameProducer.kt                     テキスト認識フォールバック
       低FPS JPEG変換 (5-15fps)           Layer 2.5: LfmClassifier (Ollama)
  └→ AiJpegSender.kt                        qwen3:0.6b / qwen3:1.7b
       TCP [int32 len][int32 w]              OCRテキスト→アクション分類
           [int32 h][int64 tsUs]           Layer 3: OllamaVision (Ollama)
           [bytes jpeg]                       llava:7b → ポップアップ検出
  └→ PC ai_port (H.264とは別ポート)           非同期実行, 全デバイス合計1並列
                                        
                                         VisionDecisionEngine
                                           状態機械: IDLE→DETECTED→CONFIRMED→COOLDOWN
                                           Layer3トリガー: 5秒マッチなし or 10秒スタック
```

---

## Android AI送信側 (android/capture/src/.../ai/ および capture/)

### AiStream.kt (capture/capture/AiStream.kt)
```kotlin
// H.264ストリームとは完全に独立したVirtualDisplayを使用
// 低解像度・低FPSで撮影 → JPEG → TCP送信
projection.createVirtualDisplay("mirage_ai", width, height, dpi, ...)
  → ImageReader(RGBA_8888)
  → AiFrameProducer(fps=1-30, quality=30-95)
  → AiJpegSender(host, port)
```

### AiFrameProducer.kt (capture/ai/AiFrameProducer.kt)
```kotlin
// ImageReader からRGBA取得 → Bitmap → JPEG圧縮
// rowStride/pixelStride 対応 (パディング考慮)
// 間欠駆動 (intervalMs = 1000/targetFps)
```

### AiJpegSender.kt (capture/ai/AiJpegSender.kt)
```kotlin
// フレームフォーマット: [int32 len][int32 w][int32 h][int64 tsUs][bytes jpeg]
// 再接続対応 (send失敗時にauto-reconnect)
// tcpNoDelay=true
```

### ScreenCaptureService での制御 (CMD経由)
```kotlin
CMD_AI_START: AiStream(proj).start(host, aiPort, w, h, dpi, fps, quality)
CMD_AI_STOP:  aiStream?.stop()
// 設定はSharedPreferencesに永続化 (PREF_AI_ENABLED等)
// maybeStartAiFromPrefs(): サービス起動時に前回設定を自動復元
// EXTRA: ai_port, ai_width, ai_height, ai_fps, ai_quality
```

---

## PC AI受信・処理側 (src/ai/, src/gui/)

### Layer 1: テンプレートマッチ (ai_engine.cpp)
- VulkanTemplateMatcher (GPU NCC)
- テンプレート: build/templates/ (現在0枚)
- FrameReadyEvent → processFrame → VisionDecisionEngine.update()

### Layer 2.5: LfmClassifier (ai/lfm_classifier.hpp/cpp) — 2026-03-06 追加
```cpp
// OCRテキストからUI操作アクションを分類
// Layer 1/2 で判断不能時のフォールバック
model_fast  = "qwen3:0.6b"   // was: LFM2-350M (English-only → 日本語非対応のため変更)
model_smart = "qwen3:1.7b"   // was: LFM2.5-1.2B
// 速度: ~20ms (vs llava:7b の ~700ms)
// アクション分類: Close / Tap / Ignore / Unknown
// Unknown → Layer 3 (llava:7b) へフォールバック
```

**AIEngine内の呼び出しフロー:**
```cpp
// ai_engine.cpp:7300付近
if (lfm_classifier_ && action.type == AIAction::Type::NONE ...) {
    auto lfm_r = lfm_classifier_->classify(ocr_text);      // fast model
    if (lfm_r.action == UiAction::Unknown) {
        lfm_r = lfm_classifier_->classifySmart(ocr_text);   // smart model
    }
}
```

### Layer 3: OllamaVision (ai/ollama_vision.hpp/cpp) — 2026-03-09 更新
```cpp
// llava:7b を使用したポップアップ検出
// RGBA → PNG (内部エンコーダ, zlib不使用のsimple PNG) → Base64 → Ollama API
// WinHTTP経由でHTTP POST (winhttp.lib)
// warmupAsync(): 起動時に非同期CLIPプリロード (初回38s問題対策)
// 同時実行: 全デバイス合計1つ (LAYER3_MAX_CONCURRENT=1)
// 応答パース: JSON {"found":true,"type":"ad","button_text":"X","x_percent":50,"y_percent":50}
// モデル: llava:7b (qwen3.5:4b はvision projectorなしのため不使用)
```

### VisionDecisionEngine (ai/vision_decision_engine.hpp)
```cpp
// 状態機械: IDLE → DETECTED → CONFIRMED → COOLDOWN → IDLE
// confirm_count = 3  (設定で変更可)
// cooldown_ms   = 2000ms
// Layer3トリガー条件:
//   layer3_no_match_frames = 150 (~5s@30fps)
//   layer3_stuck_frames    = 300 (~10s@30fps)
//   layer3_no_match_ms     = 5000ms (フレームレート非依存)
//   layer3_cooldown_ms     = 30000ms (LLM重いため長め)
// EWMA平滑化: enable_ewma=false (後方互換でデフォルトOFF)
```

### GUI初期化 (gui_init.cpp:initializeAI())
```cpp
g_ai_engine = std::make_unique<mirage::ai::AIEngine>();
// config.json から設定読込:
//   ai.templates_dir, ai.default_threshold
//   ai.vde_confirm_count, ai.vde_cooldown_ms
//   ai.vde_enable_layer3, ai.vde_layer3_* 各種閾値
//   ollama.host/port/model/timeout_ms/max_tokens
// g_ai_engine->initialize(ai_config, vk_ctx)  ← VulkanContext注入
// g_ai_engine->loadTemplatesFromDir(...)
// setCanSendCallback: USB AOA or ADB利用可能時のみアクション許可
// LfmClassifier: AIEngine内部で自動初期化 (ai_engine.cpp:2595)
```

---

## 現在のOllamaモデル構成

| 用途 | モデル | 速度 | 状態 |
|------|--------|------|------|
| Layer 2.5 fast | qwen3:0.6b | ~20ms | ✅ 設定済み |
| Layer 2.5 smart | qwen3:1.7b | ~50ms | ✅ 設定済み |
| Layer 3 vision | llava:7b | ~700ms | ✅ 設定済み (disable中) |
| Layer 3 text | llama3.1:8b | - | インストール済み |
| Layer 3 text2 | gemma2:9b | - | インストール済み |

> ※ LFM2-350M / LFM2.5-1.2B から qwen3系に変更済み (日本語対応のため)

---

## 現在の稼働状態

| コンポーネント | 状態 | 備考 |
|-------------|------|------|
| AIEngine (C++) | ✅ USE_AI有効 | gui_main.cpp #ifdef USE_AI |
| Layer 1 テンプレートマッチ | ⚠️ テンプレート0枚 | Learning Modeで追加が必要 |
| Layer 2 OCR | ✅ MIRAGE_OCR_ENABLED | FrameAnalyzer→AIEngine接続済み |
| Layer 2.5 LfmClassifier | ✅ 初期化済み | qwen3系, Ollama要起動 |
| Layer 3 OllamaVision | ❌ disable | vde_enable_layer3=false |
| Android AI送信 (AiStream) | ✅ 実装済み | CMD_AI_STARTで起動 |
| Ollama サービス | ⚠️ 手動起動要 | Session 1 (タスクトレイ) |

---

## 未確認・要対応

| 項目 | 内容 | 優先度 |
|------|------|--------|
| AiJpegReceiver (PC側) | AiJpegSender対応のTCP受信側がPC側コードで未発見 | 🔴 要確認 |
| テンプレート収集 | build/templates/が空 → GUI Learning Modeで作成 | 🟠 必要 |
| Layer 3有効化 | テンプレート揃ってから enable_layer3=true | 🟡 後回し |
| qwen3 動作確認 | Ollama起動後にLfmClassifier E2Eテスト未実施 | 🟡 要確認 |
