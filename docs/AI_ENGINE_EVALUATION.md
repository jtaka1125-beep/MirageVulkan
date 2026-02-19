# MirageVulkan AI認識エンジン 技術検証・品質評価レポート

**評価日**: 2026-02-19
**対象コミット**: cf959dc (master)
**評価者**: Claude Code (自動解析)

---

## 総合評価サマリー

| セクション | 評価 | 概要 |
|---|---|---|
| 1. コーディング規約準拠 | **B+** | namespace/MLOG統一済。Result型の適用が一部不徹底 |
| 2. アーキテクチャ整合性 | **B** | EventBus連携は堅実。UiFinder重複、AIEngine↔TemplateStore未接続 |
| 3. 機能完成度 | **B-** | GPU基盤は本番級。addTemplateFromFile未実装、VisionDecisionEngine不在 |
| 4. スレッド安全性 | **B+** | 主要箇所はmutex保護済。VulkanTemplateMatcher非スレッドセーフは設計意図 |
| 5. 未実装・スタブ・TODO | **C+** | 画像デコーダー未実装、WIC依存残存、EventBus経由フレーム処理スタブ |
| 6. テストカバレッジ | **C** | EventBus/Result/FrameAnalyzerのみ。GPU系・AI系テスト皆無 |

**総合**: **B-** — GPU基盤の完成度は高いが、AIパイプラインの統合とテストに課題

---

## 1. コーディング規約準拠 【B+】

### 1.1 namespace mirage 準拠

| ファイル | namespace | 準拠 |
|---|---|---|
| vulkan_template_matcher.hpp/cpp | `mirage::vk` | OK |
| vulkan_compute_processor.hpp/cpp | `mirage::vk` | OK |
| frame_analyzer.hpp/cpp | `mirage` | OK |
| ai_engine.hpp/cpp | `mirage::ai` | OK |
| ui_finder.hpp/cpp (src/) | `mirage` | OK |
| ui_finder.hpp (src/ai/) | `mirage::ai` | OK |
| template_manifest.hpp/cpp | `mirage::ai` | OK |
| template_store.hpp/cpp | `mirage::ai` | OK |
| template_capture.hpp/cpp | `mirage::ai` | OK |
| template_writer.hpp/cpp | `mirage::ai` | OK |
| template_autoscan.hpp/cpp | `mirage::ai` | OK |
| template_hot_reload.hpp/cpp | `mirage::ai` | OK |
| learning_mode.hpp/cpp | `mirage::ai` | OK |
| event_bus.hpp | `mirage` | OK |
| gui_command.hpp/cpp | `mirage::gui::command` | OK |
| mirage_log.hpp | `mirage::log` | OK |
| result.hpp | `mirage` | OK |

**評価**: 全ファイル100% `mirage` namespace階層に統一。

### 1.2 MLOG_* マクロ使用

- **全ファイルでMLOG_*マクロを使用**: printf/cout/std::clog の使用なし
- TAGは `static constexpr const char* TAG` で一貫
- ログレベルの使い分けが適切（INFO=通常、WARN=degraded、ERROR=失敗）
- `learning_mode.cpp:183` で文字列リテラルTAG `"learning"` を直接使用（`static constexpr` TAG未定義）— 軽微

### 1.3 RAII

- **EventBus**: `SubscriptionHandle` による確実な購読解除 — **優秀**
- **Vulkan資源**: `unique_ptr<VulkanImage>`, `unique_ptr<VulkanComputePipeline>` で管理
- **VulkanTemplateMatcher::~VulkanTemplateMatcher()**: VkBuffer/VkFence/VkCommandPool を手動解放 — 正しいが、RAII wrapperがあればさらに堅固
- **FrameAnalyzer**: TessImpl の pimpl + unique_ptr で Tesseract のライフサイクル管理 — **良好**
- **LearningMode**: mutex + SubscriptionHandle の二重保護 — 正しい

### 1.4 Result型

- `result.hpp`: Rust風 `Result<T,E>` が完成度高い（map, map_err, value_or, expect, void特殊化）
- **適用状況**:
  - `src/ai/ui_finder.hpp`: `Result<UiElement>` で全メソッド統一 — **模範的**
  - `src/ui_finder.hpp/cpp`: `std::optional<UiElement>` + `last_error_` 文字列 — **旧API未移行**
  - `ai_engine.hpp`: `bool + std::string& error` パターン — Result未適用
  - `vulkan_template_matcher.hpp`: `bool + std::string& error` — Result未適用
  - `template_store.hpp`: `bool + std::string* out_err` — Result未適用
  - `template_capture.hpp`: 独自 `CaptureResult` 構造体（ok + error フィールド）— 部分的
  - `template_writer.hpp`: 独自 `WriteResult` 構造体 — 部分的

**問題点**: Result型が定義されているのに、大半のモジュールで未採用。`src/ai/ui_finder.hpp` のみが模範。

### 1.5 日本語コメント

- **大半のファイルで日本語コメントを使用** — 規約準拠
- `vulkan_template_matcher.cpp`: 英語コメントのみ（Vulkan API寄りの低レベル層は英語が適切とも言える）
- shaders: 英語コメント（GLSL標準慣行として許容）

**総評**: namespace/MLOG/RAIIは優秀。Result型の全面適用が今後の課題。

---

## 2. アーキテクチャ整合性 【B】

### 2.1 VulkanTemplateMatcher ↔ TemplateStore

**問題: 未接続**

`VulkanTemplateMatcher` は `addTemplate(name, gray_data, w, h, group)` でCPU gray8データを受け取り、GPU側にアップロードする。`TemplateStore` は `loadFromFile()` でstbi_imageから Gray8 データを保持する。

しかし、**AIEngine::Impl は TemplateStore を使っていない**。`AIEngine::Impl::addTemplate()` は直接 `VulkanComputeProcessor::rgbaToGray()` → `VulkanTemplateMatcher::addTemplate()` を呼ぶ。

接続パスが存在するのは `template_hot_reload.cpp` の `addOrUpdateTemplateAndRegister()` のみ（Store → Matcher の橋渡し）。

```
期待: AIEngine → TemplateStore.loadFromFile → VulkanTemplateMatcher.addTemplate
実態: AIEngine → VulkanComputeProcessor → VulkanTemplateMatcher (Storeバイパス)
      template_hot_reload → TemplateStore → VulkanTemplateMatcher (別経路)
```

**影響**: テンプレート管理が二重経路になり、TemplateStore のメタデータ（matcher_id マッピング等）がAIEngine経由では未設定。

### 2.2 FrameAnalyzer ↔ AIEngine

**独立動作、統合なし**

- `FrameAnalyzer`: OCR専用。`FrameReadyEvent` 購読 → OCRテキスト抽出
- `AIEngine`: テンプレートマッチング専用。`FrameReadyEvent` 購読（ただしハンドラはスタブ）

**両者の統合がない**: AIActionの決定にOCR結果が使われていない。`ai_engine.cpp:307-311` で `onFrameReady` は空実装（「EventBus経由では重複呼び出しを避ける」コメント）。

### 2.3 EventBus連携

| イベント | 発行元 | 購読先 | 状態 |
|---|---|---|---|
| FrameReadyEvent | 外部(gui) | FrameAnalyzer, AIEngine, LearningMode | **動作** (AIEngineはスタブ) |
| LearningStartEvent | GUI | LearningMode | **動作** |
| LearningCaptureEvent | LearningMode | (未購読) | 発行のみ |
| TapCommandEvent | gui_command定義 | (未使用) | **未活用** |
| SwipeCommandEvent | gui_command定義 | (未使用) | **未活用** |
| UiFindRequestEvent | (未使用) | src/ai/UiFinder | 定義のみ |
| UiFindResultEvent | src/ai/UiFinder | (未使用) | 定義のみ |
| DeviceConnectedEvent | 外部 | (外部) | EventBus基盤テスト済 |
| ShutdownEvent | 外部 | (外部) | EventBus基盤テスト済 |

**問題**:
- `TapCommandEvent`/`SwipeCommandEvent` が定義されているが、`gui_command.cpp` では直接関数呼び出し（`g_hybrid_cmd->send_tap()`）。EventBus経由のデカップリングが未完了。
- `UiFindRequestEvent`/`UiFindResultEvent` は `src/ai/ui_finder.hpp` に定義されているが、実装（`subscribe_events()`, `on_find_request()`）が見当たらない（ヘッダ宣言のみ）。

### 2.4 UiFinder 重複

**2つのUiFinderが存在**:

1. `src/ui_finder.hpp/cpp` (namespace `mirage`) — `std::optional<UiElement>`, `last_error_` 文字列ベース
2. `src/ai/ui_finder.hpp` (namespace `mirage::ai`) — `Result<UiElement>`, EventBus連携, mutex保護

`src/ai/ui_finder.hpp` は `Result<T>` を使用しており明らかに新しい設計だが、**実装(.cpp)が存在しない**。`src/ui_finder.cpp` のみが完全実装。

**推奨**: `src/ai/ui_finder.hpp` をベースに `src/ui_finder.cpp` を移行統合するか、旧版を廃止。

### 2.5 gui_command ↔ AIEngine

`gui_command.cpp` は `sendTapCommand()` 等でUSB AOA/ADB/IPC経由のコマンド送信を実装。
`AIEngine` は `ActionCallback` でアクション実行を委譲可能。

**接続状態**: `AIEngine::setActionCallback()` に `gui_command::sendTapCommand` を渡す統合コードは確認できない。GUIからのフレーム処理→AI判定→コマンド実行のフルパイプラインは外部（gui_main.cppやgui_threads.cpp）での結線が必要。

---

## 3. 機能完成度 【B-】

### 3.1 テンプレートマッチング

| 機能 | 実装状態 | 品質 |
|---|---|---|
| GPU NCC (tile-based) | **完了** | 本番級。shared mem最適化、early exit、barrier正しい |
| SAT-based NCC | **完了** | Kahan compensated prefix sum。float32精度対策済 |
| Multi-scale pyramid | **完了** | coarse-to-fine 2段階。3レベルピラミッド |
| CPU→GPU変換 | **完了** | rgbaToGray shader + temp image upload |
| テンプレートファイル読込 | **部分的** | `addTemplateFromFile()` → "画像デコーダー未実装" 返却 |
| 複数テンプレート同時マッチ | **完了** | 1コマンドバッファで全テンプレートdispatch |
| 結果readback | **完了** | atomicCounter + host-visible buffer |
| パフォーマンス計測 | **完了** | EMA(0.9)で平均時間を追跡、100回毎にログ |

**シェーダー品質**:
- `template_match_ncc.comp`: 5-point variance early exit(Opt G)、template shared memory cache(Opt C)、tile-based processing(Opt B)。**高品質**。
- `template_match_sat.comp`: SAT query macro、template cache。SAT_QUERYマクロでbranch条件処理。cross-correlation は直接計算で残存（SAT化不可能なため適切）。
- `prefix_sum_horizontal.comp` / `prefix_sum_vertical.comp`: Kahan compensated summation で float32 精度劣化に対応。**学術的にも正確**。
- `pyramid_downsample.comp`: 2x2平均。シンプルで正確。
- `rgba_to_gray.comp`: BT.601 luma。OpenCV cvtColorと同等。
- `yuv_to_rgba.comp`: NV12→RGBA。BT.601/BT.709切替。push constant経由のcolor space選択。

### 3.2 OCR (FrameAnalyzer)

| 機能 | 実装状態 | 品質 |
|---|---|---|
| Tesseract初期化 | **完了** | LSTM_ONLY、PSM_AUTO |
| EventBus FrameReadyEvent 購読 | **完了** | フレームキャッシュ(device_id別) |
| RGBA → Leptonica PIX | **完了** | composeRGBPixel使用 |
| OCR実行 | **完了** | 単語レベルイテレーション、BBox+confidence |
| findText (部分一致) | **完了** | case-insensitive |
| getTextCenter (タップ座標) | **完了** | BBox中心座標 |
| グローバルシングルトン | **完了** | `mirage::analyzer()` |

**注意**: `#ifdef MIRAGE_OCR_ENABLED` ガード。Tesseract未インストール環境ではコンパイル除外。

### 3.3 UiFinder

| 機能 | 実装状態 | 品質 |
|---|---|---|
| resource-id検索 | **完了** | uiautomator XML dump + regex parse |
| テキスト検索 | **完了** | 部分一致/完全一致 |
| OCR検索 | **完了** | FrameAnalyzer連携 |
| 座標テーブル | **完了** | JSON load/save、デバイスモデル照合 |
| AUTO戦略 | **完了** | 4戦略順次試行 |
| タイムアウト | **完了** | 500ms間隔リトライ |
| ADB実行 | **完了** | Windows CreateProcess / Unix popen |

### 3.4 VisionDecisionEngine

**不在**: 要件に記載の `VisionDecisionEngine` は存在しない。`AIEngine::Impl::decideAction()` が相当する機能を内包しているが、クラスとして独立していない。画面状態判定は `ActionMapper::classifyState()` が担う。

### 3.5 学習モード (LearningMode)

| 機能 | 実装状態 | 品質 |
|---|---|---|
| EventBus FrameReadyEvent 購読 | **完了** | フレームキャッシュ |
| LearningStartEvent → テンプレート生成 | **完了** | ROI切出し → PNG保存 → manifest登録 |
| Gray8変換 | **完了** | BT.601 luma整数近似 |
| PNG書出し | **完了(WIC)** | Windows WIC API使用。**stb_image_writeではない** |
| manifest更新 | **完了** | allocateNextId → saveManifestJson |
| LearningCaptureEvent 発行 | **完了** | 結果publish |

### 3.6 テンプレート管理パイプライン

| コンポーネント | 実装状態 |
|---|---|
| template_manifest (JSON読み書き) | **完了** — 独自JSONパーサー |
| template_store (Gray8保持) | **完了** — stbi_load + RGBA→Gray |
| template_capture (ROI切出し) | **完了** — RGBAバッファからクランプ付き |
| template_writer (PNG保存) | **完了** — stb_image_write |
| template_autoscan (ディレクトリ走査) | **完了** — mtime比較、マニフェスト同期 |
| template_hot_reload (Store+Matcher一括更新) | **完了** — loadFromFile→addTemplate |

---

## 4. スレッド安全性 【B+】

### 4.1 mutex保護状況

| コンポーネント | mutex | 保護対象 | 評価 |
|---|---|---|---|
| EventBus | `mutex_` | handlers_ map | **安全**: publish時snapshot取得でデッドロック回避 |
| FrameAnalyzer | `frames_mutex_` | frames_ cache | **安全**: lock_guard一貫 |
| FrameAnalyzer | `ocr_mutex_` | Tesseract API | **安全**: OCR実行全体をlock |
| AIEngine::Impl | `names_mutex_` | id_to_name_ map | **安全**: コピー取得パターン |
| AIEngine::Impl | `matches_mutex_` | last_matches_ cache | **安全**: mutable mutex |
| LearningMode | `mutex_` | config_, frame_cache_, running_ | **安全**: 全操作でlock |
| mirage::log | `g_log_mutex` | stderr, log file | **安全**: write内でlock |

### 4.2 非スレッドセーフコンポーネント

- **VulkanTemplateMatcher**: ヘッダにスレッドセーフ保証なし。`cmd_buf_` を単一インスタンスで保持。同時呼び出しは未定義動作。
  - **緩和**: `VulkanComputeProcessor` ヘッダに「NOT thread-safe - caller must synchronize」と明記。設計意図として許容。
- **TemplateStore**: mutex保護なし。`loadFromFile()` と `get()` の同時呼び出しは危険。
  - **緩和**: 初期化時にのみ書き込み、実行時は読み取りのみであれば問題なし。ただしhot_reload時の同時アクセスは要注意。
- **UiFinder (src/)**: mutex保護なし。`src/ai/ui_finder.hpp` には `mutable std::mutex mutex_` あり（新設計）。

### 4.3 デッドロックリスク

- **EventBus**: publish時にmutex解放後にハンドラ呼び出し（snapshot pattern） — **デッドロック安全**
- **LearningMode**: `onLearningStart()` 内で `mutex_` lock → `bus().publish()` — EventBus内部mutexとのネスト。EventBusのpublishはsnapshot patternのためデッドロックしない。**安全**。
- **FrameAnalyzer**: `frames_mutex_` と `ocr_mutex_` の2つのmutex。`analyzeText()` では frames_mutex_ → 解放 → ocr_mutex_ の順序。**安全**（同時保持しない）。

### 4.4 競合状態

- **AIEngine::Impl::addTemplate()**: `vk_processor_->rgbaToGrayGpu()` と `vk_processor_->rgbaToGray()` を同じフレームに対して2回呼んでいる（GPU変換結果は破棄してCPU readbackで登録）。非効率だが競合はない（単一スレッド前提）。
- **VulkanTemplateMatcher**: `sat_built_` フラグがmutex保護なし。matchGpuの呼び出しが単一スレッドであれば問題なし。

---

## 5. 未実装・スタブ・TODO 【C+】

### 5.1 明示的な未実装

| 場所 | 内容 | 影響度 |
|---|---|---|
| `ai_engine.cpp:440` | `addTemplateFromFile()` → "画像デコーダー未実装" | **高**: `loadTemplatesFromDir()` が機能しない |
| `ai_engine.cpp:307-311` | `onFrameReady()` 空実装 | **中**: EventBus経由のフレーム処理パイプライン未完成 |
| `src/ai/ui_finder.hpp` | `subscribe_events()`, `on_find_request()` 宣言のみ | **中**: EventBus連携UiFinder未実装 |
| `learning_mode.cpp` | `writeGray8Png()` WIC依存 | **中**: `template_writer.hpp` (stb_image_write)と重複。WIC非RAII |

### 5.2 設計上のスタブ

| 箇所 | 状態 | 説明 |
|---|---|---|
| `AIEngine` → `TemplateStore` 接続 | **未接続** | AIEngineがTemplateStoreをバイパス |
| `AIEngine` → `FrameAnalyzer` 統合 | **未接続** | OCR結果がアクション決定に未使用 |
| `TapCommandEvent` / `SwipeCommandEvent` | **未活用** | EventBus定義あるがgui_commandは直接関数呼出し |
| `VisionDecisionEngine` | **不在** | 独立クラスとして存在しない |
| `MatchRect.w` / `MatchRect.h` | **常に0** | `VkMatchResult` にテンプレートサイズ情報がない |

### 5.3 WIC / stb_image_write 二重実装

`learning_mode.cpp` は WIC (Windows Imaging Component) で PNG 保存。
`template_writer.cpp` は stb_image_write で PNG 保存。

同じ機能の二重実装。`template_writer` が MirageComplete からの移行先として正しいが、`learning_mode` はまだ WIC を直接使用。

### 5.4 独自JSONパーサー

`template_manifest.cpp` に最小限の手書きJSONパーサー（findString, findInt, splitObjectsInArray）。エスケープ処理なし。ネストしたJSONやエスケープされた引用符を含むデータでは壊れる可能性。

---

## 6. テストカバレッジ 【C】

### 6.1 既存テスト

| テストファイル | 対象 | テスト数 | 品質 |
|---|---|---|---|
| test_event_bus.cpp | EventBus | 8 | **良好**: subscribe/publish/RAII/例外/move |
| test_result.cpp | Result<T,E> | 18 | **優秀**: Ok/Err/map/void/complex types |
| test_frame_analyzer.cpp | FrameAnalyzer/OcrResult | 9 | **良好**: findText/fullText/lifecycle/blank image |

### 6.2 未テストモジュール（AI認識エンジン関連）

| モジュール | テスト | 重要度 |
|---|---|---|
| VulkanTemplateMatcher | **なし** | **最高** — コア機能。GPUテストは困難だがモック可能 |
| VulkanComputeProcessor | **なし** | **高** — RGBA→Gray変換の正確性検証が必要 |
| AIEngine | **なし** | **高** — processFrame/decideAction/ActionMapper |
| TemplateStore | **なし** | **中** — stbi_load + Gray8管理のユニットテスト |
| TemplateManifest | **なし** | **中** — JSON読み書き、ID割当のロジックテスト |
| TemplateCapture | **なし** | **中** — ROIクランプ、Gray8変換のテスト |
| TemplateWriter | **なし** | **低** — stbi_write_pngラッパー |
| TemplateAutoscan | **なし** | **中** — ディレクトリ走査、マニフェスト同期 |
| TemplateHotReload | **なし** | **中** — Store+Matcher+Manifest統合テスト |
| LearningMode | **なし** | **中** — EventBus駆動テンプレート学習 |
| UiFinder (src/) | **なし** | **中** — XML parse、ADB実行モック |
| UiFinder (src/ai/) | **なし** | **中** — Result型API（実装なし） |
| ActionMapper | **なし** | **中** — classifyState、getAction |
| Shaders (.comp) | **なし** | **高** — NCC数値精度、SAT正確性 |

### 6.3 推奨テスト追加（優先順）

1. **TemplateManifest + TemplateStore**: CPUのみ。JSON読み書き、Gray8登録/取得。GPU不要で即座にテスト可能。
2. **TemplateCapture**: ROIクランプロジック、境界条件テスト。CPUのみ。
3. **ActionMapper**: classifyState、getActionのロジックテスト。CPUのみ。
4. **AIEngine (mock)**: VulkanTemplateMatcher/VulkanComputeProcessorをモック化してdecideActionロジックをテスト。
5. **UiFinder XML parser**: `parse_ui_dump()` / `parse_bounds()` のユニットテスト。CPUのみ。
6. **NCC shader validation**: CPU参照実装との数値比較（要GPUテストインフラ）。

---

## 付録A: ファイル一覧と行数

| ファイル | 行数 | 用途 |
|---|---|---|
| src/vulkan_template_matcher.hpp | 147 | GPU NCCマッチャー（ヘッダ） |
| src/vulkan_template_matcher.cpp | 1024 | GPU NCCマッチャー（実装） |
| src/vulkan_compute_processor.hpp | 89 | RGBA→Gray GPU変換（ヘッダ） |
| src/vulkan_compute_processor.cpp | 237 | RGBA→Gray GPU変換（実装） |
| src/frame_analyzer.hpp | 99 | Tesseract OCR（ヘッダ） |
| src/frame_analyzer.cpp | 284 | Tesseract OCR（実装） |
| src/ai_engine.hpp | 119 | AIエンジン統合（ヘッダ） |
| src/ai_engine.cpp | 571 | AIエンジン統合（実装、USE_AIガード付き） |
| src/ui_finder.hpp | 129 | UiFinder v1（ヘッダ、optional型） |
| src/ui_finder.cpp | 417 | UiFinder v1（実装） |
| src/ai/ui_finder.hpp | 172 | UiFinder v2（ヘッダ、Result型、実装なし） |
| src/ai/template_manifest.hpp | 35 | マニフェスト管理（ヘッダ） |
| src/ai/template_manifest.cpp | 181 | マニフェスト管理（JSON） |
| src/ai/template_store.hpp | 50 | テンプレートストア（ヘッダ） |
| src/ai/template_store.cpp | 127 | テンプレートストア（stbi_load） |
| src/ai/template_capture.hpp | 44 | ROI切出し（ヘッダ） |
| src/ai/template_capture.cpp | 94 | ROI切出し（実装） |
| src/ai/template_writer.hpp | 19 | PNG保存（ヘッダ） |
| src/ai/template_writer.cpp | 41 | PNG保存（stb_image_write） |
| src/ai/template_autoscan.hpp | 32 | ディレクトリスキャン（ヘッダ） |
| src/ai/template_autoscan.cpp | 162 | ディレクトリスキャン（実装） |
| src/ai/template_hot_reload.hpp | 39 | ホットリロード（ヘッダ） |
| src/ai/template_hot_reload.cpp | 128 | ホットリロード（実装） |
| src/ai/learning_mode.hpp | 106 | 学習モード（ヘッダ） |
| src/ai/learning_mode.cpp | 343 | 学習モード（WIC PNG保存） |
| src/event_bus.hpp | 210 | イベントバス（header-only） |
| src/gui/gui_command.hpp | 24 | GUIコマンド（ヘッダ） |
| src/gui/gui_command.cpp | 265 | GUIコマンド（USB/ADB/IPC） |
| src/mirage_log.hpp | 91 | 構造化ログ（header-only） |
| src/result.hpp | 257 | Result型（header-only） |
| shaders/template_match_ncc.comp | 174 | NCC compute shader |
| shaders/template_match_sat.comp | 127 | SAT-based NCC shader |
| shaders/pyramid_downsample.comp | 29 | ピラミッドダウンサンプル |
| shaders/rgba_to_gray.comp | 22 | RGBA→Gray変換 |
| shaders/yuv_to_rgba.comp | 78 | YUV(NV12)→RGBA変換 |
| shaders/prefix_sum_horizontal.comp | 80 | 水平prefix sum (SAT) |
| shaders/prefix_sum_vertical.comp | 75 | 垂直prefix sum (SAT) |

**合計**: ~5,700行 (C++/GLSL)

---

## 付録B: 改善推奨アクション（優先順）

### 即時対応 (P0)

1. **`addTemplateFromFile()` を実装**: `TemplateStore::loadFromFile()` を呼び出すように変更。stbi_loadは既にtemplate_storeに存在。
2. **AIEngine ↔ TemplateStore 接続**: `AIEngine::Impl` に `TemplateStore` メンバを追加し、`loadTemplatesFromDir` を template_autoscan + template_store 経由に変更。

### 短期 (P1)

3. **UiFinder統合**: `src/ui_finder.cpp` を `src/ai/ui_finder.hpp` の Result型インタフェースに移行。旧ヘッダ廃止。
4. **LearningMode PNG保存をtemplate_writerに統一**: WIC依存を除去し `mirage::ai::writeGray8Png()` を呼び出す。
5. **MatchRect にテンプレートサイズ情報追加**: `VkMatchResult` にサイズフィールドを追加するか、id_to_name_と同様のid_to_sizeマップを追加。
6. **テスト追加**: TemplateManifest, TemplateStore, TemplateCapture, ActionMapper のCPUユニットテスト。

### 中期 (P2)

7. **Result型の全面適用**: VulkanTemplateMatcher, AIEngine の `bool + string& error` を `Result<T>` に移行。
8. **EventBus経由コマンド実行**: TapCommandEvent/SwipeCommandEvent をgui_commandで活用。AIEngine → EventBus → gui_command のフルパイプライン。
9. **OCR統合**: FrameAnalyzerの結果をAIEngine::decideAction()に組み込み（テキスト検出によるアクション分岐）。
10. **独自JSONパーサー改善**: nlohmann/json等への置換、またはエスケープ処理の追加。
