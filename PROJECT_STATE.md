# MirageSystem Project State
# Updated: 2026-03-01 Session 3
# Read at session start, updated at session end.
# THIS IS THE MAIN REPOSITORY (MirageComplete is legacy/migrated)

## Current Phase: Migration Phase 3 COMPLETE + GUI Refactoring COMPLETE

## Active Blockers / Known Issues
- AOA full-path verification: requires physical USB connection (currently WiFi-only)
- Ollama: Session 1 (ユーザーセッション) での手動起動が必要
- X1 APKデプロイ: WiFiADB shellゾンビ問題は修正済み、次回Ollama起動後に再実行

## Architecture (Video Pipeline)
- Engine: FULLY CUSTOM. No scrcpy.
- Android TX: MediaProjection -> H264Encoder -> AnnexBSplitter -> RtpH264Packetizer
              -> UsbVideoSender (VID0 batch) / TcpVideoSender / UdpVideoSender
- PC RX (USB): HybridCommandSender.video_callback -> VID0 parse -> g_usb_decoders[device_id]
- PC RX (TCP): adb forward tcp:50100 -> MirrorReceiver.start_tcp_vid0 -> VID0 -> RTP -> decode
- Routing: RouteController (tcp_only_mode=true), FPS via ADB broadcast to MirageCapture APK
- Note: g_hybrid_receiver always nullptr at runtime; TCP+USB decoder path active

## Architecture (AI Pipeline)
### C++ AIEngine (GUI必要)
  FrameReadyEvent → AIEngine.processFrame → テンプレートマッチング(Layer1/2) → tap/swipe
  Layer3 (Ollama vision): disabled (vde_enable_layer3=false), テンプレート揃ってから有効化予定
  VDE設定: confirm_count=2, cooldown=2s, threshold=0.75
  テンプレート: build/templates/ (現在0枚 → GUIのLearning Modeで追加)

### Python MacroAPI (MCP経由)
  screenshot: MacroApiServer → jpeg_cache (GUI起動時~数ms) or ADB screencap fallback
  ai_analyze: macro_screenshot → Ollama llava:7b → テキスト分析 (E2E 30-45s)
  parallel_shot: 3台並列screenshot (threading.Thread)

### MCP Tools (37個 / 2026-03-01確認)
  主要: run_command, read_file, write_file, run_task, task_status, task_cancel
        adb_devices, adb_shell, screenshot, macro_screenshot, ai_analyze
        desktop_screenshot, git_status, status, build_mirage
        memory_search, memory_bootstrap, memory_append_raw, memory_append_decision

## Android APK Status (2 APKs)
- :capture (com.mirage.capture) - 2026/02/26 capture-release.apk
  - ScreenCaptureService, H264Encoder, RtpH264Packetizer, TcpVideoSender
  - AudioCaptureService, ML: ScreenAnalyzer, ChangeDetector, OcrEngine
- :accessory (com.mirage.accessory) - 2026/02/24 accessory-release.apk
  - AccessoryIoService (AOA USB I/O), MirageAccessibilityService
  - Protocol.kt (MIRA protocol, cmd 0x00-0x27)
  - directBootAware=true, USB_ACCESSORY_ATTACHED intent-filter

## Device Status
- WiFi: 192.168.0.3:5555 (X1/Npad X1), 192.168.0.6:5555 (A9#956), 192.168.0.8:5555 (A9#479)
- APK: A9×2 両APKデプロイ済み, X1 次回デプロイ予定
- USB: 3 devices expected (WinUSB driver required for AOA) [BLOCKED]

## MCP Server Health
- Version: v5.0.0 | PID: 32696 | Port: 3000 | Status: Running
- Transports: SSE + Streamable HTTP
- wifi_adb_guard daemon: PID 32424, 30秒間隔shellゾンビ検出・自動復旧

## Ollama Status
- Models: llava:7b (vision), llama3.1:8b (text), gemma2:9b (text)
- Status: 要手動起動 (Session 1 / タスクトレイから)
- post-commit hook: Ollama停止時はメモリ保存スキップ (エラーは無害)

## Completed 2026-03-01 Sessions 1-3
### Session 3 (AIパイプライン整備)
- server.py: tool_macro_screenshot / tool_ai_analyze 追加(MacroAPI+llava:7b)
- server.py: _macro_rpc() / _ollama_vision() ヘルパー追加
- watchdog_ai.py: ADB重複監視削除(wifi_adb_guardに一元化), adbフルパス修正
- wifi_adb_guard.py: shellゾンビ検出・30秒監視・daemon化 (MirageWiFiADB タスク)
- ai_receiver_service.py: 廃止(孤立したデッドコード)
- config.json: VDE設定・Ollama設定追加 (threshold 0.75, Layer3 disabled)
- mcp-server: 不要スクリプト削除・整理 (37ツール確認)

### Session 2 (X1接続安定化)
- wifi_adb_guard: shellゾンビ検出+30秒daemon化
- deploy_apk.py: mDNS除外・adbフルパス・タイムアウト延長
- X1 APKデプロイ: 当日は成功したが翌日再度offline (要手動復旧)
- parallel_shot.py: 3台並列screenshotクライアント

### Session 1 (Screenshot & Deploy)
- screenshot API: FrameReadyEvent購読方式 (GUI H264フレームをJPEGキャッシュ, ~数ms)
- AdbH264Receiver廃止 (screenrecordの競合問題回避)
- A9×2 APKデプロイ成功, mcp-server 222スクリプト削除

## Next Priorities (Ordered)
1. Ollama起動後: ai_analyze E2Eテスト (ai_pipeline_test.py)
2. X1 APKデプロイ: wifi_adb_guard自動復旧後に再実行
3. GUIのLearning Modeでテンプレート収集 → C++ AIEngineのLayer1/2テスト
4. AOA full-path test [BLOCKED: physical USB]

## GUI File Line Counts (Updated 2026-03-01)
- gui_ai_panel.cpp:       663
- gui_init.cpp:           837
- gui_threads.cpp:        642
- gui_device_control.cpp: 539
- gui_command.cpp:        356
- gui_window.cpp:         237
- gui_main.cpp:           295
- gui_state.cpp:           11
- mirage_context.cpp:       0
- TOTAL:                 3580 lines

## Key Decisions Log
- 2026-03-01: AIパイプライン整備完了。tool_ai_analyze(Python/Ollama)とC++ AIEngine(テンプレートマッチ)は並立。
- 2026-03-01: wifi_adb_guard daemon化。shellゾンビをget-stateでなくecho __alive__で検出。
- 2026-03-01: ai_receiver_service.py廃止。TCPポート51100-51104は未使用だった。
- 2026-03-01: config.json VDE設定追加。Layer3(Ollama)はdisabled維持(テンプレートなし)。
- 2026-03-01: screenshot API - FrameReadyEvent購読方式確定。AdbH264Receiver廃止。
- 2026-02-24: GUI refactoring COMPLETE, Migration Phase 3 COMPLETE.
- 2026-02-23: Video engine fully custom, no scrcpy.
- 2026-02-16: MirageVulkan designated as main repo (MirageComplete = legacy).

## Test Coverage (33 tests registered in CMakeLists.txt)
- TemplateStoreTest 12, ActionMapperTest 14, WinUsbCheckerTest 14
- TemplateManifestTest, BandwidthMonitorTest, HybridSenderTest, MirrorReceiverTest
- VID0ParserTest, ConfigLoaderTest, VisionDecisionEngineTest (40 cases), etc.


## Completed 2026-03-01 Session 3 (AIパイプライン整備)
### AIパイプライン
- screenshot API: FrameReadyEvent購読→jpeg_cache方式 (GUI H264から~数ms)
- MCP tool_macro_screenshot: MacroAPI経由でデバイス画面取得 (tool登録済み)
- MCP tool_ai_analyze: screenshot→llava:7b→テキスト分析 (tool登録済み、Ollama待ち)
- ai_receiver_service.py廃止 (未接続のデッドコード、MirageAIReceiverタスク無効化)
### mcp-server整備
- watchdog_ai.py: requests→urllib.request置換 (標準ライブラリのみ)
- watchdog_ai.py: ADB二重監視削除 (wifi_adb_guardに一元化)
- wifi_adb_guard: shellゾンビ検出・30秒監視・daemon化
- server.py多重起動時の旧プロセス競合問題確認 (要: MCPGuard再有効化)

## Next Priorities (Ordered)
1. Ollamaが復活したらtool_ai_analyze E2Eテスト (screenshot→llava:7b→結果)
2. MCP server多重起動防止強化 (MirageMCPGuardを再有効化)
3. AOA full-path test [BLOCKED: physical USB]
4. Multi-device video pipeline stress test [BLOCKED: physical USB]