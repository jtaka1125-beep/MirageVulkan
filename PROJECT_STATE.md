# MirageSystem Project State
# Updated: 2026-03-13
# Read at session start, updated at session end.
# THIS IS THE MAIN REPOSITORY (MirageComplete is legacy/migrated)

## Current Phase: Migration Phase 3 COMPLETE + GUI Refactoring COMPLETE

## Active Blockers / Known Issues
- AOA full-path verification: requires physical USB connection (currently WiFi-only)
- Ollama: Session 1 (ユーザーセッション) での手動起動が必要
- AiJpegReceiver (PC側): 実装済み (src/ai/ai_jpeg_receiver.cpp), 統合未完了

## Architecture (Video Pipeline)
- Engine: FULLY CUSTOM. No scrcpy.
- Codec: **H.265/HEVC** (2026-03-10確認、実機APK・ソースコード両方確認済み)
- Android TX: MediaProjection -> H264Encoder(HEVC) -> AnnexBSplitter
              -> RtpH264Packetizer(useHevc=true, RFC 7798 FU-A)
              -> UsbVideoSender (VID0 batch) / TcpVideoSender / UdpVideoSender
- PC RX (USB): HybridCommandSender.video_callback -> VID0 parse -> g_usb_decoders[device_id]
- PC RX (TCP): adb forward tcp:50100 -> MirrorReceiver.start_tcp_vid0 -> VID0 -> RTP -> decode
- PC Decode: mirror_receiver.cpp stream_is_hevc_自動検出 -> UnifiedDecoder(VideoCodec::HEVC)
             -> h264_decoder.cpp(use_hevc=true, FFmpeg AV_CODEC_ID_HEVC)
             ※ Vulkan Video はH.264のみ対応、HEVCはFFmpegフォールバック
- Routing: RouteController (tcp_only_mode=true), FPS via ADB broadcast to MirageCapture APK
- Note: g_hybrid_receiver always nullptr at runtime; TCP+USB decoder path active
- Detail: VIDEO_PIPELINE_STATUS.md 参照

## Architecture (AI Pipeline)
> Detail: AI_STATUS.md 参照 (2026-03-10 実コード確認済み)

### C++ AIEngine (GUI必要, #ifdef USE_AI)
  FrameReadyEvent → AIEngine.processFrame
  Layer 1:   VulkanTemplateMatcher (GPU NCC) テンプレート0枚, Learning Mode未実施
  Layer 2:   OCR (Tesseract) ← FrameAnalyzer→AIEngine接続済み
  Layer 2.5: LfmClassifier ← 2026-03-06追加
             モデル: qwen3:0.6b (fast, ~20ms) / qwen3:1.7b (smart, ~50ms)
             ※旧: LFM2-350M / LFM2.5-1.2B → 英語専用のためqwen3系に変更
             OCRテキスト→アクション(Close/Tap/Ignore)分類, Unknown→Layer3フォールバック
  Layer 3:   OllamaVision (llava:7b) ← disabled (vde_enable_layer3=false)
             warmupAsync()でCLIPプリロード対応済み (初回38s問題対策)
             ※ qwen3.5:4b はvision projectorなしのため不使用
  VDE設定: confirm_count=3, cooldown_ms=2000
           Layer3トリガー: 5s無マッチ or 10s同一テンプレートスタック, cooldown=30s
  テンプレート: build/templates/ (現在6枚 → GUIのLearning Modeで追加)

### Android AIストリーム (AiStream.kt) ← 2026-03-10確認, 実装済み
  H.264とは独立したVirtualDisplay → ImageReader(RGBA_8888) → JPEG → TCP送信
  AiFrameProducer.kt: 低FPS(1-30fps指定), JPEG品質30-95, rowStride/pixelStride対応
  AiJpegSender.kt: [int32 len][int32 w][int32 h][int64 tsUs][bytes jpeg], auto-reconnect
  ScreenCaptureService: CMD_AI_START/STOP, SharedPreferences永続化, 起動時自動復元
  ※ PC側のAiJpeg受信コードは未確認 → 要調査

### Python MacroAPI (MCP経由)
  screenshot: MacroApiServer → jpeg_cache (GUI起動時~数ms) or ADB screencap fallback
  ai_analyze: macro_screenshot → Ollama llava:7b → テキスト分析 (E2E 30-45s)
  parallel_shot: 3台並列screenshot (threading.Thread)

### Ollama モデル構成 (2026-03-10確認)
  Layer 2.5 fast:  qwen3:0.6b  (~20ms)   ← was LFM2-350M (英語専用)
  Layer 2.5 smart: qwen3:1.7b  (~50ms)   ← was LFM2.5-1.2B
  Layer 3 vision:  llava:7b    (~700ms)  disable中
  installed:       llama3.1:8b, gemma2:9b (text用, 未使用)

### MCP Tools (37個 / 2026-03-01確認)
  主要: run_command, read_file, write_file, run_task, task_status, task_cancel
        adb_devices, adb_shell, screenshot, macro_screenshot, ai_analyze
        desktop_screenshot, git_status, status, build_mirage
        memory_search, memory_bootstrap, memory_append_raw, memory_append_decision

## Android APK Status
- :capture (com.mirage.capture) - X1インストール済み 2026-03-10 01:26更新
  - versionCode=1, versionName=1.0.0, targetSdk=33
  - ScreenCaptureService, H264Encoder(HEVC), RtpH264Packetizer(useHevc=true)
  - TcpVideoSender, UsbVideoSender(VID0), UdpVideoSender
  - AiStream / AiFrameProducer / AiJpegSender (AIサブストリーム)
  - AudioCaptureService, ML: ScreenAnalyzer, ChangeDetector, OcrEngine
  - BootReceiver: WiFi ADB port 55555固定 (2026-03-08追加)
  - ※ソース未コミット変更あり (H264Encoder.kt HEVC移行等)
- :accessory (LEGACY - merged to :capture on 2026-03-08, no longer built)
  - AccessoryIoService (AOA USB I/O), MirageAccessibilityService
  - Protocol.kt (MIRA protocol, cmd 0x00-0x27)
  - directBootAware=true, USB_ACCESSORY_ATTACHED intent-filter

## Device Status (2026-03-10確認)
| デバイス | シリアル | 接続 | APK | 状態 |
|---------|---------|------|-----|------|
| Npad X1 | 93020523431940 | USB | capture v1.0.0 | ✅ Connected |
| Npad X1 | 192.168.0.3:5555 | WiFi | (同上) | ✅ Connected |
| RebotAi A9 (.6) | 192.168.0.6:5555 | WiFi | 両APK済み | ❌ Offline (手元なし) |
| RebotAi A9 (.8) | 192.168.0.8:5555 | WiFi | 両APK済み | ❌ Offline (手元なし) |
※ X1はUSB+WiFi同時接続で2台表示されるが同一デバイス

## MCP Server Health (2026-03-10確認)
- Version: v5.0.0 | Port: 3000 | Status: Healthy ✅
- Uptime: 71h / Requests: 20,121 / Errors: 6 (0.0%)
- Transports: SSE + Streamable HTTP
- wifi_adb_guard daemon: 30秒間隔shellゾンビ検出・自動復旧

## Ollama Status
- Models: llava:7b (vision), llama3.1:8b (text), gemma2:9b (text)
- Status: 要手動起動 (Session 1 / タスクトレイから)
- post-commit hook: Ollama停止時はメモリ保存スキップ (エラーは無害)

## MirageMemory 外部記憶 (2026-03-13 v2完成)
- 状態: **実用水準（評価 8.5 相当）**
- v2 により、MirageMemory は append-only な会話保存層から、状態遷移を伴う知識管理層へ移行した。
- commit: 58c5a12 (mcp-server/memory_store.py, server.py, test_memory_v2.py)
- DB stats (mirage-infra): raw 2018 / decision 59 / fact 6 / todo 6 / total 2089
- v2 主な変更:
  - decision 構造化（9カラム追加: decision_text/rationale/status/supersedes/superseded_by等）
  - compact_store_extracted() — compact結果をfact/todo/risk/decisionとして永続化
  - LIKE dedup — FTS5非依存の重複候補検出
  - supersedes chain — 旧decisionをsupersededに遷移させる寿命管理
  - search() CJK fallback — 日本語短文でFTS5が空振りする問題を修正
- 受け入れ確認: test_memory_v2.py 全8テスト通過、cleanup 12/12 ✅
- 残件: git push（帰宅後SSH）、Phase 2 compact dedup（同NS/24h/同文字列スキップ）

## Next Priorities (Ordered)
1. ✅ AnnexBSplitter.kt HEVC - RtpH264Packetizerで正しく処理済み (2026-03-11確認)
2. ✅ H.265 E2E - X1 HEVC 1080x1800@60fps, TCP接続確立, GUI表示OK (2026-03-11確認)
3. ✅ git commit - 未コミット変更一括コミット完了 (2026-03-11, 7件)
4. ✅ LfmClassifier - qwen3:0.6b + /no_think モード動作確認 (2026-03-11)
5. ✅ TileCompositor - 9dd4b29で削除済み (不要)
6. ✅ MirageMemory v2 実装完了 (2026-03-13, commit 58c5a12)
7. GUIのLearning Modeでテンプレート収集 → AIEngine Layer1テスト
8. AOA full-path test [BLOCKED: physical USB]
9. MirageMemory Phase 2: compact dedup

## GUI File Line Counts (Updated 2026-03-13)
- gui_ai_panel.cpp:       815
- gui_init.cpp:          1280
- gui_threads.cpp:        888
- gui_device_control.cpp: 554
- gui_command.cpp:        490
- gui_window.cpp:         237
- gui_main.cpp:           362
- gui_state.cpp:           11
- mirage_context.cpp:       0
- TOTAL:                 4637 lines

## Key Decisions Log
- 2026-03-13: MirageMemory v2 実装完了。decision構造化・compact再格納・lifecycle管理・受け入れ確認まで一巡完了。append-only保存層から状態遷移を伴う知識管理層へ移行。commit 58c5a12。
- 2026-03-13: USBLAN(RNDIS)復旧完了。前回セッションが誤ってtcp_hostを削除していた問題を修正。X1: tcp_host=10.189.194.30, preferred_route=tcp。TCP動画14Mbps、遅延<1ms。コミット33ec0ed。
- 2026-03-11: AiJpegReceiver実装（PC側AIストリーム受信）。MirageContext統合。
- 2026-03-11: VERIFYING状態追加（アクション後検証+リトライ）。テンプレート無視リスト追加（永続化+GUI編集）。
- 2026-03-11: H.265 E2E確認完了。AnnexBSplitter/RtpH264Packetizer HEVC対応確認。LfmClassifier動作確認。TileCompositor削除確認。検出オーバーレイ可視化実装。座標スケーリング実装。7件コミット。
- 2026-03-10: AIパイプライン実コード確認。Layer 2.5 LfmClassifierモデルをLFM2系→qwen3系に変更済み(日本語対応)。AiStream.kt(Android側AIサブストリーム)実装確認。AI_STATUS.md新規作成。
- 2026-03-10: ビデオコーデックをH.265/HEVCに確認。実機APK(X1)・ソースコード(MirageVulkan)を直接調査。PC受信側はHEVC自動検出+FFmpegデコード対応済み。VIDEO_PIPELINE_STATUS.md新規作成。
- 2026-03-08: BootReceiver追加、WiFi ADB port 55555固定。X1 APK 2026-03-10更新確認。
- 2026-03-06: LfmClassifier (Layer 2.5) 追加。モデルはqwen3:0.6b/1.7b。
- 2026-03-03: MCP二重起動防止完了。MirageMCPServerタスクをDisable化、start_all.batをMirageMCP(watchdog)経由に統一、MirageMCPGuard(1分毎ヘルスチェック)を再有効化。
- 2026-03-03: TileCompositor実装・コミット(1d8daa3)。native_h>1440デバイス(X1)で自動タイルモード起動。
- 2026-03-01: AIパイプライン整備完了。tool_ai_analyze(Python/Ollama)とC++ AIEngine(テンプレートマッチ)は並立。
- 2026-03-01: wifi_adb_guard daemon化。shellゾンビをget-stateでなくecho __alive__で検出。
- 2026-03-01: ai_receiver_service.py廃止。TCPポート51100-51104は未使用だった。
- 2026-03-01: screenshot API - FrameReadyEvent購読方式確定。AdbH264Receiver廃止。
- 2026-02-24: GUI refactoring COMPLETE, Migration Phase 3 COMPLETE.
- 2026-02-23: Video engine fully custom, no scrcpy.
- 2026-02-16: MirageVulkan designated as main repo (MirageComplete = legacy).

## Test Coverage (33 tests registered in CMakeLists.txt)
- TemplateStoreTest 12, ActionMapperTest 14, WinUsbCheckerTest 14
- TemplateManifestTest, BandwidthMonitorTest, HybridSenderTest, MirrorReceiverTest
- VID0ParserTest, ConfigLoaderTest, VisionDecisionEngineTest (40 cases), AiJpegReceiverTest (5 cases), etc.
