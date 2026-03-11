# MirageSystem — Claude Code Instructions

## Project Overview
MirageVulkan: Windows-based Android device automation and screen mirroring platform.
USB AOA + WiFi hybrid connectivity, up to 20 simultaneous devices, Vulkan GPU H.264 decode, AI screen analysis.
This is the main repository (successor to MirageComplete, full upper-compatible).

## Repository Map
- **MirageVulkan** (C:\MirageWork\MirageVulkan) — MAIN: C++ core, Android APKs, scripts, shaders
- **mcp-server** (C:\MirageWork\mcp-server) — MCP server for Claude.ai integration, ADB management
- **MirageComplete** (C:\MirageWork\MirageComplete) — LEGACY: being migrated into MirageVulkan

## Architecture
- **C++ GUI** (src/, ~90 files): Vulkan + ImGui, Vulkan Video H.264 decode, libusb AOA, EventBus
- **GPU Shaders** (shaders/, 7 .comp files): YUV->RGBA, NCC template matching, pyramid downsampling
- **Android APKs** (android/): com.mirage.capture (all-in-one: video+AOA+AI) [1APK since 2026-03-08]
- **Scripts** (scripts/, 31 files): deploy_apk.py, device_health.py, mirage_cli.py, bt_auto_pair.py
- **MCP Server** (../mcp-server/): server.py (SSE+Streamable HTTP), task_queue.py, watchdog.py

## Build
- **C++**: Visual Studio 2022, CMake 4.x, Vulkan SDK 1.4+, FFmpeg
  cmake .. -G "Visual Studio 17 2022" -A x64
  cmake --build . --config Release
- **Tests**: 16 suites, all PASS: ctest --output-on-failure
- **APK**: python scripts/deploy_apk.py

## Absolute Rules
1. ALWAYS read the full file before modifying large files (server.py=1900+ lines, gui_render.cpp=938 lines)
2. ALWAYS verify build after code changes (cmake --build . --config Release)
3. NEVER break existing tests (16 suites must all PASS)
4. Japanese comments preferred for log messages and inline comments
5. Use MLOG_* macros for logging (printf/cout/std::clog prohibited)

## Coding Conventions
- RAII throughout (mutex/lock_guard/unique_lock)
- Thread safety: g_running atomic, 153+ mutex/lock points
- Error handling: try-catch required, WinMain fully protected
- Inter-module communication via EventBus (no direct cross-module references)
- Result<T> type for error propagation

## Device Environment
- USB: Npad X1 (TAB005) on ReTRY HUB port 2-2
- WiFi ADB: 192.168.0.3:5555 (Npad X1), 192.168.0.6:5555 (A9), 192.168.0.8:5555 (A9)
- USB Hub: ReTRY HUB (retryhub_ctrl.exe, output cp932 encoding)
- Supported: Npad X1 (Android 13, 1200x2000), A9 (Android 15, 800x1340)

## Communication Design
- 2 channels (control + video) x 2 pipes (USB + WiFi)
- USB priority; bandwidth pressure -> video to WiFi -> FPS reduction fallback
- AOA requires no USB debugging (security by design)
- Single APK: com.mirage.capture (merged 2026-03-08)

## Key Decisions
- scrcpy-server for MediaProjection bypass (Android 15)
- SSE + Streamable HTTP hybrid (Claude.ai connector uses SSE)
- MirageComplete -> MirageVulkan migration (Phase 1-3 planned)
- Multi-agent: Director(Opus) + Worker(Sonnet) + Reviewer(Sonnet read-only)

## データフロー図

### 映像パイプライン (Android → PC)
```
[Android]
ScreenCaptureService.kt → H264Encoder.kt → RtpH264Packetizer.kt
    ↓ USB (VID0)              ↓ WiFi (TCP:50100)
[PC]
usb_video_receiver.cpp    mirror_receiver.cpp
         ↓                       ↓
      VID0 parse → RTP parse → unified_decoder.cpp
                                    ↓
                        vulkan_video_decoder.cpp (GPU)
                        h264_decoder.cpp (FFmpeg fallback)
                                    ↓
                        vulkan_texture.cpp → ImGui表示
```

### コマンドパイプライン (PC → Android)
```
[PC]
gui_input.cpp (マウス/キー) → hybrid_command_sender.cpp
                                    ↓
                        mirage_protocol.hpp (MIRA packet)
    ↓ USB (AOA)                     ↓ WiFi (ADB)
[Android]
AccessoryIoService.kt           adb shell input
         ↓
    Protocol.kt (parse)
         ↓
MirageAccessibilityService.kt (実行)
```

### AIパイプライン
```
gui_ai_panel.cpp → ai_engine.cpp → vulkan_template_matcher.cpp (GPU NCC)
                        ↓
              vision_decision_engine.cpp → action_mapper.hpp
                        ↓
              hybrid_command_sender.cpp (自動操作)
```

## キーファイルマップ

### タスク別ファイル早見表
| やりたいこと | 触るファイル |
|-------------|-------------|
| タップ/スワイプ修正 | `hybrid_command_sender.cpp`, `Protocol.kt` (両方) |
| 映像が映らない | `mirror_receiver.cpp`, `unified_decoder.cpp` |
| USB接続問題 | `usb_device_discovery.cpp`, `aoa_protocol.cpp` |
| GUI表示修正 | `src/gui/gui_*.cpp`, `gui_render_*.cpp` |
| AI認識精度 | `vulkan_template_matcher.cpp`, `template_store.cpp` |
| FPS/ビットレート | `H264Encoder.kt`, `route_controller.cpp` |
| 新コマンド追加 | `mirage_protocol.hpp` + `Protocol.kt` (3箇所同期必須) |

### Protocol.kt 同期必須ファイル (新コマンド追加時)
1. `src/mirage_protocol.hpp` — PC側定義
2. `android/accessory/.../Protocol.kt` — accessoryモジュール
3. `android/capture/.../Protocol.kt` — captureモジュール

### 触ってはいけないファイル
| ファイル | 理由 |
|---------|------|
| `src/stb_image.h`, `stb_image_write.h` | サードパーティ (283KB) |
| `android/app/*` | レガシー、settings.gradle.ktsで除外済み |
| `third_party/*` | 外部ライブラリ |
| `*.spv` | コンパイル済みシェーダー (自動生成) |

### ディレクトリ構成
```
src/
├── ai/          # AIエンジン (template_*, vision_*, ui_finder)
├── video/       # デコーダ (vulkan_video_*, h264_*, unified_*)
├── vulkan/      # Vulkanインフラ (context, swapchain, texture)
├── gui/         # GUI (init, threads, command, device_control, ai_panel)
└── *.cpp/hpp    # 通信・コア (mirror_receiver, hybrid_*, adb_*)

android/
├── capture/     # com.mirage.capture (映像送信, ML)
├── accessory/   # [LEGACY] com.mirage.accessory (merged to capture 2026-03-08, AOA, コマンド受信)
└── app/         # [除外] レガシーモノリス

scripts/         # Python ユーティリティ
tools/           # デバッグ用パッチ (本番コードではない)
shaders/         # GLSL compute (*.comp → *.spv)
tests/           # GoogleTest (33テスト)
docs/            # 設計ドキュメント
```

## 既知の地雷

### USB/ADB競合
- **MirageVulkan起動中はUSB ADBにX1が出ない** — AOAがUSBを横取りするため
- **WinUSBドライバ(PID_201C)とADBドライバ(PID_200C)は排他的** — 切り替えにはデバイスマネージャーでドライバ変更が必要
- **AOA接続中のデバイスはadb devicesに表示されない** — これは仕様

### WiFi ADB
- **tcpip 5555は再起動で消える** — `wifi_adb_devices.conf`で管理、`wifi_adb_guard.py`が自動再接続
- **IPアドレス変わるとWiFi ADB切れる** — DHCPリース更新時に注意

### Android 15特有
- **MediaProjection毎回許可必要** — Android 15の仕様変更、自動化できない
- **A9デバイスはスクショ取得に8秒以上かかることがある** — WiFi ADB経由の制約

### ビルド/署名
- **testsigningモード必須** — WinUSBドライバ署名のため、`bcdedit /set testsigning on`
- **APKはrelease署名必須** — debug署名だとAccessoryが動かない（AOA認証に影響）

### プロトコル
- **Protocol.kt 3箇所同期必須** — PC側、accessory、captureの3つ、1つでも漏れると通信失敗
- **MIRA packetはリトルエンディアン** — Javaのデフォルト(BE)と逆なので注意

### TCPポート割り当てルール
- **単一ソース: `build/devices.json`** — ここだけ変更すれば全体に反映される
- **割り当て式: `50100 + slot × 2`** (slot = devices.json内の登録順 0始まり)
  - slot 0 → X1 (192.168.0.3): primary=**50100**, secondary=50101
  - slot 1 → A9 (192.168.0.6): primary=**50102**, secondary=50103
  - slot 2 → A9 (192.168.0.8): primary=**50104**, secondary=50105
- **PC側**: `autoStartCaptureService()` が `EXTRA_TCP_PORT` としてintentに渡す
- **Android側**: `ScreenCaptureService.onStartCommand()` で受け取り、`tcpPort+1` をセカンダリに使用
- **新デバイス追加時**: devices.jsonに `"tcp_port": 50100 + slot×2` を設定するだけでOK

### Hub/電源
- **ReTRY HUBのポートサイクルは1秒以上待つ** — 短すぎると認識失敗
- **X1はUSBハブ経由だと電力不足になることがある** — 直結推奨
- **デバイスoffline時の復旧**: 個別ポートではなくサブハブ全体(Port:2)をOFF→ONする


## IP/Port 二段構えルール（BuildConfig + Intent + 自動割当）

### 目的
- 通常運用は「起動するだけ」で IP/Port をデフォルト適用（手間削減）
- 例外時は PC→Android の Intent extra で従来通り上書き可能
- 未確定でも衝突しないように自動割当し、決まったら固定化して以降は確認だけで回す

### 採用優先順位（必須）
1. Intent extra（PC側から渡す値）
2. BuildConfig に焼き込んだデフォルト（APKビルド時設定）
3. 自動割当（未確定・未設定時のみ）
4. 最後の保険のハードコード（可能なら排除）

### 対象パラメータ
- TCP ミラー: tcp_port（※TCPミラーではIP不要：ADB forwardでPC localhost接続）
- UDP ミラー: host / port

### Build時デフォルト（APKに焼き込む）
- BuildConfig.DEFAULT_TCP_PORT
- BuildConfig.DEFAULT_HOST
- BuildConfig.DEFAULT_UDP_PORT

### 実行時上書き（現行維持）
- Intent extra:
  - tcp_port（--ei tcp_port）
  - host（--es host）
  - port（--ei port）
- ただし通常運用では渡さなくても動く（= デフォルトが効く）

### 未確定時の自動割当（衝突回避 + 固定化）
- 自動割当が発動する条件:
  - extra が無い（0/未設定）かつ BuildConfig も未設定（0/未設定）

#### TCPポート自動割当（推奨）
- base = 50100
- step = 2（偶数のみ使用。port+1 を二次用途に空けやすい）
- 初期候補: p = base + slot*step

slot の決め方（推奨順）
- A: devices.json に slot を持つ（最優先・安定）
- B: hardware_id / usb_serial を hash して slot 化（自動・安定）

衝突回避（必須）
- 衝突判定:
  - devices.json の既存 tcp_port（予約済み）
  - PC側 LISTEN 中ポート（実使用中）
- 衝突したら p += step で空きを探索して採用（探索上限を設ける）

固定化（重要）
- 自動割当で決めた tcp_port は devices.json に書き戻して固定化
- 次回以降は同一端末が同一ポートを使う（運用は確認だけ）

### IPの未確定時の扱い
- 端末識別は hardware_id / usb_serial を主キーにする（IPはDHCPで変動しうる）
- 接続時に得た adb_id（ip:5555）から最新IPを devices.json に更新して追従

### “確認だけで済む”ログ要件
起動時に必ず1行ログで以下を出す:
- default（BuildConfig）/ extra（Intent）/ auto（自動割当）/ use（採用値）/ 衝突解決回数（shift）

例:
- CFG tcp: build=50100 extra=0 auto=50104 shift=2 -> use=50104 | udp: build=192.168.0.2:50000 extra=:0 -> use=...
