# MirageSystem v2 コード監査レポート

**作成日:** 2026-02-07
**対象:** C:\MirageWork\MirageComplete\src\ 全ソースファイル + CMakeLists.txt
**監査対象ファイル数:** 70+ ファイル (*.cpp, *.hpp, *.h)
**総行数:** 約 12,000+ 行

---

## 目次

1. [プロジェクトアーキテクチャ概要](#1-プロジェクトアーキテクチャ概要)
2. [データフロー分析](#2-データフロー分析)
3. [ファイル別分析](#3-ファイル別分析)
4. [ビルド構成 (CMakeLists.txt)](#4-ビルド構成-cmakeliststxt)
5. [機能完成度マトリクス](#5-機能完成度マトリクス)
6. [映像表示実現のためのTODOリスト](#6-映像表示実現のためのtodoリスト)

---

## 1. プロジェクトアーキテクチャ概要

### 1.1 システム全体像

MirageSystem v2は、複数のAndroidデバイスをPC上で一括制御・映像表示するためのWindows GUIアプリケーションである。

```
┌──────────────────────────────────────────────────────────────────────┐
│                       PC側 (Windows C++ GUI)                         │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │                 ImGui + D3D11 描画レイヤー                    │    │
│  │  gui_render.cpp / gui_input.cpp / gui_application.hpp        │    │
│  │  3パネル構成: 左(操作) / 中央(メイン映像) / 右(サブ映像)     │    │
│  └──────────────┬───────────────────────────────────────────────┘    │
│                  │                                                    │
│  ┌──────────────┴───────────────────────────────────────────────┐    │
│  │            GUIモジュール層 (gui/ ディレクトリ)                 │    │
│  │  gui_state    : グローバル状態管理                             │    │
│  │  gui_command  : タップ/スワイプ命令ディスパッチ                │    │
│  │  gui_window   : Win32ウィンドウプロシージャ                    │    │
│  │  gui_threads  : バックグラウンドスレッド (ADB検出, 更新)       │    │
│  │  gui_device_control : AOA/ADBデバイス制御UI                   │    │
│  └──────────────┬───────────────────────────────────────────────┘    │
│                  │                                                    │
│  ┌──────────────┴───────────────────────────────────────────────┐    │
│  │              通信・受信プロトコル層                            │    │
│  │                                                               │    │
│  │  映像受信:                                                    │    │
│  │    MirrorReceiver     : UDP RTP H.264受信+デコード            │    │
│  │    UsbVideoReceiver   : USB AOAバルク転送映像受信             │    │
│  │    HybridReceiver     : USB/WiFi自動切替受信                  │    │
│  │    MultiDeviceReceiver: 複数デバイス同時受信                  │    │
│  │                                                               │    │
│  │  コマンド送信:                                                │    │
│  │    UsbCommandSender      : 単一デバイスUSB AOA送信            │    │
│  │    MultiUsbCommandSender : 複数デバイスUSB AOA送信            │    │
│  │    WifiCommandSender     : UDP経由WiFi送信                    │    │
│  │    HybridCommandSender   : USB/WiFiアダプタ層                 │    │
│  │                                                               │    │
│  │  IPC:                                                         │    │
│  │    MirageIpcClient : 名前付きパイプ (miraged.exe通信)         │    │
│  └──────────────┬───────────────────────────────────────────────┘    │
│                  │                                                    │
│  ┌──────────────┴───────────────────────────────────────────────┐    │
│  │                デバイス管理・AI・自動化層                     │    │
│  │  AdbDeviceManager  : ADBデバイス検出・重複排除・ポート管理    │    │
│  │  AutoSetup         : アクセシビリティ・画面共有自動設定       │    │
│  │  UiFinder          : UI要素検索 (リソースID/テキスト/OCR)     │    │
│  │  AIEngine          : GPU テンプレートマッチング (OpenCL)      │    │
│  │  OCREngine         : Tesseract OCR (日英対応)                 │    │
│  │  ConfigLoader      : config.json設定読込                      │    │
│  └──────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │              GPU/描画サブシステム                              │    │
│  │  GpuTemplateMatcherD3D11 : D3D11 コンピュートシェーダ照合     │    │
│  │  GpuTemplateMatcherMVP   : MVP版テンプレートマッチャー        │    │
│  │  TemplateStore           : テンプレートGPUストレージ          │    │
│  │  d3d11_texture_upload    : テクスチャアップロードユーティリティ│    │
│  │  wic_image_loader        : WIC画像読込 (PNG/JPG/BMP)         │    │
│  │  VideoTexture            : 動的D3D11テクスチャラッパー        │    │
│  └──────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │              音声サブシステム                                  │    │
│  │  AudioReceiver : 音声受信 (Opus/RAW PCM対応)                  │    │
│  │  AudioPlayer   : WASAPI再生 (48kHz ステレオ 16bit)            │    │
│  └──────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────┘
        │ USB AOA (映像 + コマンド)     │ WiFi/UDP (映像 + コマンド)
        │ ADB (デバイス管理)            │ IPC (miraged.exe)
        ▼                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│                    Android デバイス (複数台)                          │
│  MirageClient APK → 画面キャプチャ → H.264エンコード → RTP送信      │
│  AOA受信 → タップ/スワイプ/キー入力実行                              │
└──────────────────────────────────────────────────────────────────────┘
```

### 1.2 技術スタック

| 分類 | 技術 |
|------|------|
| GUI | ImGui + DirectX 11 (Win32) |
| 映像デコード | FFmpeg (libavcodec, libavutil, libswscale) |
| USB通信 | libusb-1.0 (Android Open Accessory Protocol) |
| AI/画像認識 | OpenCL 1.2 + OpenCV + D3D11 コンピュートシェーダ |
| OCR | Tesseract + Leptonica |
| 音声 | Windows WASAPI + Opus (オプション) |
| 画像I/O | Windows Imaging Component (WIC) |
| ビルド | CMake 3.16+, C++17 |
| ネットワーク | Winsock2 (UDP) |
| IPC | Windows名前付きパイプ |

### 1.3 スレッドモデル

| スレッド | 機能 | ファイル |
|----------|------|----------|
| メインスレッド | ImGui描画、Win32メッセージ処理、D3D11操作 | gui_main.cpp, main_gui.cpp |
| ADB検出スレッド | 起動時デバイス検出 (1回) | gui_threads.cpp |
| デバイス更新スレッド | 映像フレーム取得、統計更新 (16ms周期) | gui_threads.cpp |
| UDP受信スレッド | WiFi映像RTPパケット受信 (per MirrorReceiver) | mirror_receiver.cpp |
| USB受信スレッド | USB AOA映像データ受信 | usb_video_receiver.cpp |
| USBコマンド送信スレッド | コマンドキューからUSB送信 | multi_usb_command_sender.cpp |
| USBコマンド受信スレッド | ACK・映像データ受信 (per デバイス) | multi_usb_command_sender.cpp |
| WiFiコマンド送受信 | UDP コマンド送受信 | wifi_command_sender.cpp |
| 音声再生スレッド | WASAPI PCM再生 | audio_player.cpp |
| 画面キャプチャ開始 | 非同期で全デバイスにスクリーンキャプチャ指示 | gui_main.cpp |

---

## 2. データフロー分析

### 2.1 映像パイプライン: Android → PC画面表示

```
Android                    PC
───────                    ──────
画面キャプチャ(MediaProjection)
    │
    ▼
H.264エンコード
    │
    ├─── WiFi経路 ───────────────────────────────────────────────┐
    │    RTPパケット化                                            │
    │    UDP送信 (port 60000+N)                                  │
    │                                 MirrorReceiver::receive_thread()
    │                                 │  UDP recv() (100msタイムアウト)
    │                                 │  process_rtp_packet()
    │                                 │  ├─ RTPヘッダパース (V2検証, CSRC)
    │                                 │  ├─ NALタイプ判定
    │                                 │  │  ├─ Single NAL (1-23)
    │                                 │  │  ├─ STAP-A (24) → SPS/PPS分離
    │                                 │  │  └─ FU-A (28) → フラグメント再構築
    │                                 │  └─ decode_nal()
    │                                 │     ├─ SPS/PPSキャッシュ
    │                                 │     ├─ AnnexB変換 (00 00 00 01 + NAL)
    │                                 │     └─ H264Decoder::decode()
    │                                 │        ├─ avcodec_send_packet()
    │                                 │        ├─ avcodec_receive_frame()
    │                                 │        └─ convert_frame_to_rgba()
    │                                 │           (SwsContext YUV→RGBA)
    │                                 │
    │                                 ▼
    │                          current_frame_ (MirrorFrame: RGBA)
    │                                 │
    ├─── USB AOA経路 ────────────────┐│
    │    VID0プロトコル包装            ││
    │    [MAGIC:4][LENGTH:4][RTP]     ││
    │    USBバルク転送                 ││
    │                                 ││
    │    UsbVideoReceiver             ││
    │    │ または                      ││
    │    HybridCommandSender          ││
    │    (set_video_callback)         ││
    │    │                            ││
    │    ▼                            ││
    │    VID0パケットパース            ││
    │    ├─ マジックナンバー検証        ││
    │    ├─ 長さフィールド読込          ││
    │    └─ RTPパケット抽出            ││
    │        │                        ││
    │        ▼                        ││
    │    g_usb_decoders[device_id]    ││
    │    (MirrorReceiver::feed_rtp_packet)
    │        │                        ││
    │        ▼                        ▼│
    │    MirrorFrame (RGBA)   ────────┘│
    │                                   │
    └───────────────────────────────────┘
                    │
                    ▼
         deviceUpdateThread() [16ms周期]
         │  get_latest_frame() per デバイス
         │
         ▼
    GuiApplication::queueFrame()
    │  RGBAデータをスレッドセーフキューにコピー
    │  (MAX_PENDING_FRAMES = 30)
    │
    ▼ [メインスレッドのみ]
    GuiApplication::processPendingFrames()
    │  最新フレームのみ処理 (デバイスごと)
    │
    ▼
    updateDeviceFrame()
    │  createDeviceTexture() (D3D11 DYNAMIC テクスチャ作成/リサイズ)
    │  updateDeviceTexture() (Map/Write/Unmap)
    │
    ▼
    renderDeviceView()
    │  ImGui::GetWindowDrawList()->AddImage(texture_srv)
    │  アスペクト比保持描画
    │
    ▼
    ImGui_ImplDX11_RenderDrawData()
    SwapChain::Present()
```

### 2.2 コマンドパイプライン: GUI → Android

```
ユーザー操作                    PC                          Android
───────────                    ──────                       ───────
マウスクリック
(WndProc WM_LBUTTONUP)
    │
    ▼
onMouseUp() [gui_input.cpp]
    │  パネル判定 (Center/Right)
    │  ドラッグ距離判定 → タップ or スワイプ
    │
    ├─ タップの場合 ────────┐
    │  processMainViewClick()│
    │  screenToDeviceCoords() (画面座標→デバイス座標変換)
    │  tap_callback_()       │
    │                        │
    └─ スワイプの場合 ──────┐│
       processSwipe()       ││
       screenToDeviceCoords()│(始点・終点)
       swipe_callback_()    ││
                            ▼▼
            sendTapCommand() / sendSwipeCommand() [gui_command.cpp]
                    │
                    ├─── USB AOA経路 (優先) ──────────┐
                    │    g_hybrid_cmd                   │
                    │    →send_tap(device_id, x, y)    │
                    │    →MultiUsbCommandSender         │
                    │      build_packet()               │
                    │      [MAGIC:4][VER:1][CMD:1]      │
                    │      [SEQ:4][LEN:4][PAYLOAD]      │
                    │      queue_command()               │
                    │      send_thread() → send_raw()   │
                    │      libusb_bulk_transfer()  ──────┼──→ AOA受信
                    │                                    │     コマンド実行
                    │    ←ACK受信                        │     (タップ/スワイプ)
                    │    device_receive_thread()         │
                    │                                    │
                    ├─── WiFi UDP経路 ───────────────────┤
                    │    WifiCommandSender               │
                    │    →send_tap() via sendto()   ─────┼──→ UDP受信
                    │                                    │     コマンド実行
                    │                                    │
                    └─── IPC フォールバック ─────────────┘
                         MirageIpcClient
                         →request_once(JSON)
                         名前付きパイプ → miraged.exe → デバイス
```

### 2.3 プロトコル仕様

#### USBコマンドプロトコル (MIRA)
```
ヘッダ (14バイト):
  [0-3]  MAGIC:   0x4D495241 ("MIRA" LE)
  [4]    VERSION: 0x01
  [5]    CMD:     コマンドコード
  [6-9]  SEQ:     シーケンス番号 (LE)
  [10-13] LEN:    ペイロード長 (LE)

コマンドコード:
  0x00 CMD_PING          : ペイロードなし
  0x01 CMD_TAP            : x(4) y(4) screen_w(4) screen_h(4) pad(4) = 20B
  0x02 CMD_BACK           : ペイロードなし
  0x03 CMD_KEY            : keycode(4) flags(4) = 8B
  0x04 CMD_CONFIG         : 設定データ
  0x05 CMD_CLICK_ID       : len(2LE) + resource_id(UTF-8)
  0x06 CMD_CLICK_TEXT     : len(2LE) + text(UTF-8)
  0x07 CMD_SWIPE          : x1(4) y1(4) x2(4) y2(4) duration_ms(4) = 20B
  0x10 CMD_AUDIO_FRAME    : 音声フレームデータ
  0x80 CMD_ACK            : seq(4) status(1)
```

#### USB映像プロトコル (VID0)
```
パケット:
  [0-3]  MAGIC:  0x56494430 ("VID0" BE)
  [4-7]  LENGTH: RTPデータ長 (BE)
  [8-N]  DATA:   RTPパケット (12-65535バイト)
```

---

## 3. ファイル別分析

### 3.1 エントリポイント・GUIコア

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `gui/gui_main.cpp` | 445 | **v2エントリポイント (現行)**: モジュラー構造。初期化→メインループ→クリーンアップ | 完成 | USE_AI, USE_OCR |
| `main_gui.cpp` | 1369 | **v1エントリポイント (非推奨)**: 全機能含むモノリシック実装。参考用に保持 | 完成 (DEPRECATED) | USE_FFMPEG, USE_AI, USE_OCR, USE_LIBUSB |
| `gui_application.hpp` | 459 | **GUIアプリケーション定義**: DeviceInfo, GuiConfig, LearningSession, レイアウト定数 | 完成 | なし |
| `gui_application.cpp` | 633 | **GUIアプリケーション実装 Part1**: D3D11初期化、ImGuiセットアップ、デバイス管理、フレームキュー、学習モード | 完成 | なし |
| `gui_render.cpp` | 1036 | **GUIアプリケーション実装 Part2**: 3パネル描画、デバイスビュー、オーバーレイ、スクリーンショット表示 | 完成 | なし |
| `gui_input.cpp` | 531 | **GUIアプリケーション実装 Part3**: マウス入力、座標変換、タップ/スワイプ処理、キーボードショートカット | 完成 | なし |

### 3.2 GUIモジュール (gui/ ディレクトリ)

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `gui/gui_state.hpp` | 138 | グローバル状態宣言: レシーバー、デバイスマネージャ、AI/OCRエンジン | 完成 | USE_AI, USE_OCR, MIRAGE_DEBUG |
| `gui/gui_state.cpp` | 133 | 状態初期化・クリーンアップ: スマートポインタ管理、mutex保護 | 完成 | USE_AI, USE_OCR |
| `gui/gui_command.hpp` | 22 | コマンド関数宣言: sendTap/Swipe/getDeviceIdFromSlot | 完成 | なし |
| `gui/gui_command.cpp` | 134 | コマンドディスパッチ: USB優先→IPCフォールバック、日本語ログ | 完成 | なし |
| `gui/gui_window.hpp` | 14 | ウィンドウプロシージャ宣言 | 完成 | なし |
| `gui/gui_window.cpp` | 208 | WndProc: 16:9アスペクト比強制、最小960x540、ImGui入力転送 | 完成 | なし |
| `gui/gui_threads.hpp` | 18 | バックグラウンドスレッド宣言 | 完成 | なし |
| `gui/gui_threads.cpp` | 370 | ADB検出スレッド + デバイス更新スレッド (16ms + 500ms統計) | 完成 | USE_AI |
| `gui/gui_device_control.hpp` | 80 | AOA/ADBデバイス制御インターフェース、DeviceControlInfo | 完成 | なし |
| `gui/gui_device_control.cpp` | 405 | AOAモード切替 (aoa_switch.exe)、ADB接続管理、入力バリデーション、ImGui描画 | 完成 | なし |

### 3.3 映像受信・デコード

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `mirror_receiver.hpp` | 111 | UDP RTP H.264受信インターフェース。RFC 3984対応、FU-Aフラグメント再構築 | 完成 | USE_FFMPEG |
| `mirror_receiver.cpp` | 503 | **完全実装**: Winsock参照カウント、RTPヘッダパース、STAP-A/FU-Aデパケタイズ、SPS/PPSキャッシュ、テストパターン生成 | 完成 | USE_FFMPEG, _WIN32 |
| `h264_decoder.hpp` | 102 | FFmpeg H.264デコーダインターフェース。RAII、低遅延設定 | 完成 | なし (FFmpegリンク前提) |
| `h264_decoder.cpp` | 268 | **完全実装**: AV_CODEC_FLAG_LOW_DELAY, zerolatency, SwsContext YUV→RGBA変換、エラー追跡 | 完成 | なし |
| `video_texture.hpp` | 48 | D3D11動的テクスチャラッパー。RGBA更新、SRV取得 | 完成 | なし |
| `video_texture.cpp` | 81 | D3D11 DYNAMIC テクスチャ作成、Map/Write/Unmap更新、ピッチ処理 | 完成 | なし |
| `usb_video_receiver.hpp` | 97 | USB AOAビデオ受信。VID0プロトコル定義、バッファ管理 | 完成 | USE_LIBUSB |
| `usb_video_receiver.cpp` | 288 | libusb AOA映像受信: マルチPID対応、1.5秒フラッシュ、同期回復、128KBオーバーフロー制御 | 完成 | USE_LIBUSB |
| `hybrid_receiver.hpp` | 181 | USB/WiFiハイブリッド受信。帯域幅モニタリング、状態機械 | 完成 | なし |
| `hybrid_receiver.cpp` | 332 | **完全実装**: 100ms統計更新、USB輻輳検出(3基準)、フレームデバウンス、3秒クールダウン | 完成 | なし |
| `multi_device_receiver.hpp` | 106 | 複数デバイス映像同時受信コーディネータ | 完成 | なし |
| `multi_device_receiver.cpp` | 222 | デバイスごとMirrorReceiver管理、FPS/帯域幅計算、2秒アクティビティ検出 | 完成 | なし |

### 3.4 コマンド送信

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `usb_command_sender.hpp` | 147 | 単一デバイスUSB AOA制御。MIRAプロトコル定義、AOAモード切替 | 完成 | USE_LIBUSB |
| `usb_command_sender.cpp` | 625 | **完全実装**: libusb初期化、AOAデバイス検出(14メーカー)、3秒再列挙待機、バルクエンドポイント設定 | 完成 | USE_LIBUSB |
| `multi_usb_command_sender.hpp` | 222 | 複数デバイスUSB AOA管理。エラー追跡、ホットプラグ | 完成 | USE_LIBUSB |
| `multi_usb_command_sender.cpp` | 1205 | **最大ファイル**: デバイスごと受信スレッド、エラー回復、タイムアウト付シャットダウン、13+メーカー対応 | 完成 | USE_LIBUSB |
| `wifi_command_sender.hpp` | 123 | UDP WiFiコマンド送信。MIRAプロトコル互換 | 完成 | _WIN32 |
| `wifi_command_sender.cpp` | 396 | UDP ソケット送受信、Pingレイテンシ計測、クロスプラットフォーム | 完成 | (プラットフォーム分岐) |
| `hybrid_command_sender.hpp` | 86 | マルチデバイスコマンド送信アダプタ | 完成 | なし |
| `hybrid_command_sender.cpp` | 183 | MultiUsbCommandSenderラッパー、コールバック委譲、レガシーAPI互換 | 完成 | なし |

### 3.5 デバイス管理・ADB

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `adb_device_manager.hpp` | 137 | ADBデバイスマネージャインターフェース。UniqueDevice重複排除 | 完成 | なし |
| `adb_device_manager.cpp` | 714 | **完全実装**: デバイス検出、ハードウェアID(プライバシーハッシュ)、ポート割当、入力シミュレーション、スクリーンショット、セキュリティバリデーション | 完成 | _WIN32 |
| `adb_security.hpp` | 162 | ADBコマンドインジェクション防止ライブラリ | 完成 | なし |
| `config_loader.hpp` | 195 | config.json読込 (手書きJSONパーサー、外部依存なし)。ネットワーク/GUI/AI/OCR設定 | 完成 | なし |

### 3.6 自動化・UI検索

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `auto_setup.hpp` | 268 | アクセシビリティサービス・画面共有自動設定インターフェース | 完成 | なし |
| `auto_setup.cpp` | 667 | **完全実装**: 5段階セットアップ、複数フォールバック戦略、画面サイズ比例座標計算、10+メーカー対応 | 完成 | _WIN32 |
| `ui_finder.hpp` | 172 | マルチ戦略UI要素検索 (リソースID/テキスト/OCR/座標テーブル) | 完成 | USE_OCR |
| `ui_finder.cpp` | 408 | uiautomator XML解析、座標テーブルJSON管理、リトライロジック | 完成 (PNGデコーダ未実装) | USE_OCR, _WIN32 |

### 3.7 AI・テンプレートマッチング

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `ai_engine.hpp` | 94 | AIエンジンインターフェース。テンプレートマッチング、アクション生成 | 完成 | なし |
| `ai_engine.cpp` | 459 | **完全実装**: OpenCLコンテキスト初期化、RGBA→グレースケール、GPUマッチング、VisionDecisionEngine統合 | 完成 | USE_AI |
| `ai/gpu_template_matcher_mvp.cpp` | 374 | D3D11コンピュートシェーダMVP版テンプレートマッチャー (SAD) | 完成 | なし |
| `gpu/gpu_template_matcher_d3d11.cpp` | 340 | D3D11 SAD テンプレートマッチャー。8x8スレッドグループ、ROI対応 | 完成 | なし |
| `ai/template_store.cpp` | 113 | テンプレートGPUストレージ。WIC読込、Gray8/RGBA8変換 | 完成 | _WIN32 |
| `ai/template_manifest.cpp` | 169 | マニフェストJSON管理 (手書きパーサー)。テンプレートID割当 | 完成 | なし |
| `ai/template_autoscan.cpp` | 150 | テンプレートディレクトリ自動スキャン。新規/更新/削除検出 | 完成 | なし |
| `ai/template_capture.cpp` | 165 | D3D11テクスチャからROI切出しGray8変換 | 完成 | なし |
| `ai/template_hot_reload.cpp` | 108 | テンプレートホットリロード。ファイル→GPU登録 | 完成 | なし |
| `ai/template_writer.cpp` | 125 | WIC経由Gray8 PNG書込み | 完成 | _WIN32 |
| `ai/learning_mode.cpp` | 90 | テンプレート学習 (フレームROIからGray8抽出→保存→登録) | 完成 | なし |

### 3.8 OCR

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `ocr_engine.hpp` | 97 | OCRエンジンインターフェース。多言語対応、ROI認識、テキスト検索 | 完成 | なし |
| `ocr_engine.cpp` | 332 | **完全実装**: Tesseract LSTM、tessdataパス自動検出、ヒストグラム平坦化前処理 | 完成 | USE_OCR |

### 3.9 音声

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `audio/audio_receiver.cpp` | 154 | 音声受信デコード。Opus/RAW PCM対応、タイムスタンプヘッダ解析 | 完成 | USE_OPUS |
| `audio/audio_player.cpp` | 340 | WASAPI再生。48kHz/16bit/ステレオ、キュー管理、バッファアンダーラン検出 | 完成 | _WIN32 |
| `audio_integration_example.hpp` | 154 | 音声統合ラッパー。AudioReceiver+AudioPlayer統合 | 完成 | IMGUI_VERSION |

### 3.10 IPC・その他

| ファイル | 行数 | 役割 | 実装状態 | 条件コンパイル |
|----------|------|------|----------|----------------|
| `ipc_client.hpp` | 28 | 名前付きパイプIPCクライアント (miraged.exe通信) | 完成 | なし |
| `ipc_client.cpp` | 123 | CreateFileW→WriteFile→ReadFile、1MBキャップ | 完成 | _WIN32 |
| `aoa_switch.cpp` | 221 | AOAモード切替CLIツール。14メーカーVID対応 | 完成 | USE_LIBUSB |
| `test_screen_capture.cpp` | 72 | 画面キャプチャテスト。ADBデバイス検出→キャプチャ開始 | 完成 | なし |
| `s_inst_integration_patch.hpp` | 218 | S-inst安定性メトリクス可視化 | 完成 | なし |
| `stb_image.h` | - | stb_image ヘッダオンリーライブラリ (PNG/JPG読込) | サードパーティ | なし |
| `io/wic_image_loader.cpp` | 126 | WIC画像読込 (RGBA8/Gray8) | 完成 | _WIN32 |
| `render/d3d11_texture_upload.cpp` | 106 | D3D11テクスチャ作成ユーティリティ | 完成 | なし |

---

## 4. ビルド構成 (CMakeLists.txt)

### 4.1 ルートCMakeLists.txt (メインビルド)

```
プロジェクト: mirage_gui
C++17, CMake 3.16+

オプション (デフォルト):
  USE_FFMPEG  = ON   ← H.264デコーダ (映像表示に必須)
  USE_AI      = ON   ← AI画像認識エンジン
  USE_OCR     = ON   ← Tesseract OCR
  USE_LIBUSB  = ON   ← USB AOA映像・コマンド
  USE_OPUS    = OFF  ← Opus音声デコーダ
  BUILD_TESTS = OFF  ← Google Testユニットテスト
```

### 4.2 リンクライブラリ

| ライブラリ | 条件 | 用途 |
|-----------|------|------|
| d3d11, dxgi, d3dcompiler | 常時 | DirectX 11 描画 |
| dwmapi | 常時 | ウィンドウ管理 |
| ws2_32 | 常時 | Winsockネットワーク |
| ole32, uuid | 常時 | WASAPI音声 |
| libavcodec, libavutil, libswscale | USE_FFMPEG | H.264デコード |
| OpenCL | USE_AI | GPU計算 |
| opencv4 | USE_AI | 画像処理 |
| tesseract, lept | USE_OCR | OCR |
| libusb-1.0 | USE_LIBUSB | USB AOA通信 |
| opus | USE_OPUS | 音声デコード |

### 4.3 ビルドターゲット

| ターゲット | 種別 | 説明 |
|-----------|------|------|
| `mirage_gui` | WIN32 実行ファイル | メインGUIアプリ (コンソール非表示) |
| `mirage_gui_debug` | コンソール実行ファイル | デバッグ用 (stderr出力表示) |
| `test_screen_capture` | コンソール実行ファイル | 画面キャプチャテスト |
| `aoa_switch` | コンソール実行ファイル | AOAモード切替ツール (USE_LIBUSB時のみ) |
| `test_adb_security` | テスト実行ファイル | ADBセキュリティテスト (BUILD_TESTS時のみ) |

### 4.4 コンパイラ設定

```
定義: UNICODE, _UNICODE, WIN32_LEAN_AND_MEAN, NOMINMAX
UTF-8: MSVC /utf-8, GCC -finput-charset=UTF-8
ImGui: third_party/imgui (Win32 + DX11バックエンド)
```

---

## 5. 機能完成度マトリクス

### 5.1 総合評価

| 機能 | 実装状態 | 備考 |
|------|:--------:|------|
| **ImGui GUI** | :white_check_mark: 完成 | 3パネル構成、日本語フォント、学習モード、スクリーンショット表示 |
| **D3D11描画** | :white_check_mark: 完成 | 動的テクスチャ、リサイズ対応、VSync |
| **ADBデバイス管理** | :white_check_mark: 完成 | 検出、重複排除、ポート割当、セキュリティバリデーション |
| **USB AOAコマンド** | :white_check_mark: 完成 | 単一/複数デバイス、全ジェスチャ対応、ACK応答 |
| **WiFiコマンド** | :white_check_mark: 完成 | UDP MIRAプロトコル、レイテンシ計測 |
| **USB映像受信** | :white_check_mark: 完成 | VID0プロトコル、libusb バルク転送、同期回復 |
| **WiFi映像受信 (RTP)** | :white_check_mark: 完成 | RFC 3984、FU-A再構築、SPS/PPSキャッシュ |
| **H.264デコード** | :white_check_mark: 完成 | FFmpeg低遅延、YUV→RGBA変換 |
| **ハイブリッド自動切替** | :white_check_mark: 完成 | 帯域幅モニタリング、輻輳検出、フレームデバウンス |
| **マルチデバイス映像** | :white_check_mark: 完成 | デバイスごとMirrorReceiver、統計計算 |
| **AI テンプレートマッチング** | :white_check_mark: 完成 | OpenCL + D3D11シェーダ、学習モード、ホットリロード |
| **OCR (テキスト認識)** | :white_check_mark: 完成 | Tesseract LSTM、日英対応、ROI認識 |
| **学習モード** | :white_check_mark: 完成 | クリック記録、相対位置計算、テンプレート学習 |
| **音声再生** | :white_check_mark: 完成 | WASAPI 48kHz、Opus/PCM対応 |
| **自動セットアップ** | :white_check_mark: 完成 | アクセシビリティ、画面共有、マルチメーカー対応 |
| **デバイス制御UI** | :white_check_mark: 完成 | AOA切替、ADB接続、バッテリー表示 |
| **IPC (miraged)** | :white_check_mark: 完成 | 名前付きパイプ、JSONプロトコル |
| **セキュリティ** | :white_check_mark: 完成 | コマンドインジェクション防止、入力バリデーション |

### 5.2 条件コンパイル依存機能

| 機能 | フラグ | 無効時の動作 |
|------|--------|-------------|
| H.264デコード | `USE_FFMPEG` | テストパターン表示(カラーバー) |
| AI テンプレートマッチング | `USE_AI` | "AI not compiled in" メッセージ |
| OCR | `USE_OCR` | 空結果を返す |
| USB AOA映像/コマンド | `USE_LIBUSB` | start()がfalse返却 |
| Opus音声 | `USE_OPUS` | RAW PCMのみ |

---

## 6. 映像表示実現のためのTODOリスト

### 6.1 現状分析

コードレベルでは映像パイプラインの全コンポーネントが**完全に実装済み**である:

```
Android H.264 RTP送信
    ↓
MirrorReceiver (UDP受信 + RTPデパケタイズ) ← 完成
    ↓
H264Decoder (FFmpeg低遅延デコード → RGBA) ← 完成
    ↓
GuiApplication::queueFrame (スレッドセーフキュー) ← 完成
    ↓
processPendingFrames → updateDeviceTexture (D3D11) ← 完成
    ↓
renderDeviceView → ImGui AddImage ← 完成
```

### 6.2 映像が表示されない場合の確認項目 (最短経路)

映像表示に必要な条件を優先度順に列挙する:

#### STEP 1: ビルド確認 (必須)

- [ ] **FFmpegがリンクされているか確認**
  ```bash
  # ビルドログに以下が含まれるか確認
  -- FFmpeg enabled: avcodec;avutil;swscale
  ```
  - `USE_FFMPEG=ON` (デフォルトON) が必要
  - pkg-config で FFmpeg が見つかる必要がある
  - 対象ファイル: `CMakeLists.txt:81-155`

- [ ] **libusb がリンクされているか確認** (USB映像使用時)
  ```bash
  -- USB AOA enabled: libusb-1.0
  ```

#### STEP 2: Android側設定

- [ ] **MirageClient APKがインストール・起動されているか**
  - APKが画面キャプチャ (MediaProjection) を開始し、H.264 RTPストリームを送信する必要がある
  - 対象: `auto_setup.cpp` の `start_screen_capture()` が自動で行う

- [ ] **画面共有権限が承認されているか**
  - Android 10+では画面共有ダイアログの承認が必要
  - 対象: `auto_setup.cpp` の `approve_screen_share_dialog()` が自動で行う

- [ ] **送信先IPとポートが正しいか**
  - `config.json` の `network.pc_ip` がPC側のIPアドレスであること
  - `network.video_base_port` (デフォルト60000) が正しいこと
  - 対象: `config_loader.hpp:131-185`

#### STEP 3: ネットワーク/USB接続

- [ ] **WiFi映像: UDPポートがファイアウォールで開放されているか**
  - ポート `60000+N` (N=デバイス番号)
  - Windows Firewallで `mirage_gui.exe` のインバウンドUDP許可

- [ ] **USB映像: AOAモードに切り替わっているか**
  - `aoa_switch.exe --all` を実行、またはGUI左パネルの「全デバイス AOA切替」ボタン
  - WinUSBドライバがインストールされている必要あり
  - 対象: `gui_device_control.cpp` の `switchAllDevicesToAOA()`

- [ ] **USB映像: WinUSBドライバがインストールされているか**
  - AOAデバイス (VID=0x18D1, PID=0x2D00-0x2D05) にWinUSBドライバが必要
  - GUI左パネル「ドライバ設定」→「AOAドライバ ウィザード」で設定可能

#### STEP 4: config.json の存在確認

- [ ] **config.json が正しい場所にあるか**
  ```json
  {
    "network": {
      "pc_ip": "192.168.x.x",
      "video_base_port": 60000,
      "command_base_port": 50000
    }
  }
  ```
  - 検索パス: `../config.json` → `config.json` → `../../config.json`
  - 見つからない場合はデフォルト値使用 (pc_ip空だと受信開始不可)

#### STEP 5: デバッグ確認

- [ ] **mirage_gui_debug (コンソール版) でログ確認**
  - 以下のログが出力されるか確認:
    ```
    [Main] ADB検出完了、GUI起動中...
    [Main] マルチ受信: N台で開始
    [Main] スクリーンキャプチャ開始: N/N台
    [Main] 初回フレーム描画成功
    ```
  - フレームが到着しているか:
    - `deviceUpdateThread` 内で `queueFrame()` が呼ばれるか
    - `processPendingFrames()` でテクスチャ更新されるか

### 6.3 よくある問題と対策

| 問題 | 原因 | 対策 |
|------|------|------|
| テストパターン(カラーバー)のみ表示 | `USE_FFMPEG` が無効、またはFFmpegライブラリ未リンク | `cmake -DUSE_FFMPEG=ON` 確認、FFmpeg開発パッケージインストール |
| 「デバイス未選択」表示 | ADBデバイスが検出されていない | `adb devices` でデバイス確認、USBデバッグ有効化 |
| デバイスは表示されるが映像なし (プレースホルダ) | Android側でRTPストリームが送信されていない | APKインストール確認、画面共有権限承認、PC IPアドレス確認 |
| USB映像なし | AOAモード未切替、ドライバ未インストール | AOA切替ボタン押下、WinUSBドライバインストール |
| WiFi映像なし | ファイアウォール、IPアドレス不一致 | ファイアウォール設定、config.jsonのpc_ip確認 |
| フレームが固まる | ネットワーク障害、デコードエラー | stderr/ログでエラー確認、SPS/PPS再送確認 |

### 6.4 最短実現手順まとめ

**WiFi経由の最短パス:**

1. FFmpegをインストール・リンク確認 (`USE_FFMPEG=ON`)
2. `config.json` にPC IP設定
3. Androidデバイスで `adb devices` 確認
4. `mirage_gui_debug.exe` 起動
5. GUIの「全て更新」でスクリーンキャプチャ自動開始
6. ファイアウォールでUDP 60000+ を許可

**USB経由の最短パス:**

1. FFmpeg + libusb をインストール・リンク確認
2. `aoa_switch.exe --all` でAOAモード切替
3. WinUSBドライバインストール (ドライバウィザード使用)
4. `mirage_gui_debug.exe` 起動
5. GUI左パネル「全デバイス AOA切替」ボタン押下

### 6.5 コード上の制限事項

1. **miraged.exeが不在でもGUIは起動する** - IPC接続失敗はログのみ、映像受信は別経路で動作
2. **テストパターン生成** (`mirror_receiver.cpp:generate_test_frame()`) - FFmpeg無効時、640x480アニメーションカラーバーが表示される。これが表示されれば描画パイプラインは正常
3. **フレームキュー上限30** - 高フレームレート時に古いフレームがドロップされるが、最新フレーム優先のため映像品質に影響なし
4. **D3D11テクスチャ操作はメインスレッド限定** - `processPendingFrames()` が必ずメインスレッドから呼ばれる設計

---

## 付録A: ファイル一覧と行数

| # | パス | 行数 | 状態 |
|---|------|------|------|
| 1 | gui/gui_main.cpp | 445 | 完成 |
| 2 | gui/gui_state.hpp | 138 | 完成 |
| 3 | gui/gui_state.cpp | 133 | 完成 |
| 4 | gui/gui_command.hpp | 22 | 完成 |
| 5 | gui/gui_command.cpp | 134 | 完成 |
| 6 | gui/gui_window.hpp | 14 | 完成 |
| 7 | gui/gui_window.cpp | 208 | 完成 |
| 8 | gui/gui_threads.hpp | 18 | 完成 |
| 9 | gui/gui_threads.cpp | 370 | 完成 |
| 10 | gui/gui_device_control.hpp | 80 | 完成 |
| 11 | gui/gui_device_control.cpp | 405 | 完成 |
| 12 | gui_application.hpp | 459 | 完成 |
| 13 | gui_application.cpp | 633 | 完成 |
| 14 | gui_render.cpp | 1036 | 完成 |
| 15 | gui_input.cpp | 531 | 完成 |
| 16 | main_gui.cpp | 1369 | 完成 (非推奨) |
| 17 | mirror_receiver.hpp | 111 | 完成 |
| 18 | mirror_receiver.cpp | 503 | 完成 |
| 19 | h264_decoder.hpp | 102 | 完成 |
| 20 | h264_decoder.cpp | 268 | 完成 |
| 21 | video_texture.hpp | 48 | 完成 |
| 22 | video_texture.cpp | 81 | 完成 |
| 23 | usb_video_receiver.hpp | 97 | 完成 |
| 24 | usb_video_receiver.cpp | 288 | 完成 |
| 25 | hybrid_receiver.hpp | 181 | 完成 |
| 26 | hybrid_receiver.cpp | 332 | 完成 |
| 27 | multi_device_receiver.hpp | 106 | 完成 |
| 28 | multi_device_receiver.cpp | 222 | 完成 |
| 29 | usb_command_sender.hpp | 147 | 完成 |
| 30 | usb_command_sender.cpp | 625 | 完成 |
| 31 | multi_usb_command_sender.hpp | 222 | 完成 |
| 32 | multi_usb_command_sender.cpp | 1205 | 完成 |
| 33 | wifi_command_sender.hpp | 123 | 完成 |
| 34 | wifi_command_sender.cpp | 396 | 完成 |
| 35 | hybrid_command_sender.hpp | 86 | 完成 |
| 36 | hybrid_command_sender.cpp | 183 | 完成 |
| 37 | adb_device_manager.hpp | 137 | 完成 |
| 38 | adb_device_manager.cpp | 714 | 完成 |
| 39 | adb_security.hpp | 162 | 完成 |
| 40 | config_loader.hpp | 195 | 完成 |
| 41 | auto_setup.hpp | 268 | 完成 |
| 42 | auto_setup.cpp | 667 | 完成 |
| 43 | ui_finder.hpp | 172 | 完成 |
| 44 | ui_finder.cpp | 408 | 完成* |
| 45 | ai_engine.hpp | 94 | 完成 |
| 46 | ai_engine.cpp | 459 | 完成 |
| 47 | ocr_engine.hpp | 97 | 完成 |
| 48 | ocr_engine.cpp | 332 | 完成 |
| 49 | ipc_client.hpp | 28 | 完成 |
| 50 | ipc_client.cpp | 123 | 完成 |
| 51 | aoa_switch.cpp | 221 | 完成 |
| 52 | test_screen_capture.cpp | 72 | 完成 |
| 53 | audio/audio_receiver.cpp | 154 | 完成 |
| 54 | audio/audio_player.cpp | 340 | 完成 |
| 55 | audio_integration_example.hpp | 154 | 完成 |
| 56 | s_inst_integration_patch.hpp | 218 | 完成 |
| 57 | ai/gpu_template_matcher_mvp.cpp | 374 | 完成 |
| 58 | ai/learning_mode.cpp | 90 | 完成 |
| 59 | ai/template_autoscan.cpp | 150 | 完成 |
| 60 | ai/template_capture.cpp | 165 | 完成 |
| 61 | ai/template_hot_reload.cpp | 108 | 完成 |
| 62 | ai/template_writer.cpp | 125 | 完成 |
| 63 | ai/template_manifest.cpp | 169 | 完成 |
| 64 | ai/template_store.cpp | 113 | 完成 |
| 65 | gpu/gpu_template_matcher_d3d11.cpp | 340 | 完成 |
| 66 | io/wic_image_loader.cpp | 126 | 完成 |
| 67 | render/d3d11_texture_upload.cpp | 106 | 完成 |
| 68 | stb_image.h | - | サードパーティ |

\* ui_finder.cppのスクリーンショットPNGデコーダは未実装 (空データ返却)

---

## 付録B: セキュリティ評価

| 項目 | 評価 | 詳細 |
|------|------|------|
| ADBコマンドインジェクション防止 | :white_check_mark: 良好 | ホワイトリスト文字検証、正規表現パターンブロック、シェルエスケープ |
| デバイスID検証 | :white_check_mark: 良好 | 最大64文字、英数字+限定記号のみ |
| ファイルパス制限 | :white_check_mark: 良好 | `/data/local/tmp/` と `/sdcard/` のみ許可 |
| プライバシー | :white_check_mark: 良好 | Android IDハッシュ化 (8文字+多項式ハッシュ) |
| バッファオーバーフロー | :white_check_mark: 良好 | NAL 2MB制限、出力1MB制限、USB 128KB制限 |
| GUI入力座標 | :warning: 注意 | 座標範囲の明示的バリデーションなし (GUI層が暗黙的に制限) |
| JSONパーシング | :warning: 注意 | 手書きパーサー (不正入力に脆弱な可能性) |
| IPC認証 | :warning: 注意 | 名前付きパイプに認証・署名なし (ローカル前提) |

---

## 付録C: スレッドセーフティ評価

| リソース | 保護方法 | 評価 |
|----------|----------|------|
| `devices_` (デバイスマップ) | `std::mutex devices_mutex_` | :white_check_mark: |
| `logs_` (ログキュー) | `std::mutex logs_mutex_` | :white_check_mark: |
| `pending_frames_` (フレームキュー) | `std::mutex pending_frames_mutex_` | :white_check_mark: |
| `main_view_rect_` (ビュー矩形) | `std::mutex view_rect_mutex_` | :white_check_mark: |
| `learning_session_` | `std::mutex learning_mutex_` | :white_check_mark: |
| `g_usb_decoders` | `std::mutex g_usb_decoders_mutex` | :white_check_mark: |
| `g_usb_video_buffers` | `std::mutex g_usb_video_buffers_mutex` | :white_check_mark: |
| `g_running` | `std::atomic<bool>` | :white_check_mark: |
| `g_adb_ready` | `std::atomic<bool>` | :white_check_mark: |
| `g_registered_usb_devices` | `std::mutex g_registered_devices_mutex` | :white_check_mark: |
| `g_multi_devices_added` | `std::mutex g_multi_devices_mutex` | :white_check_mark: |
| `g_error_counters` | `std::mutex g_error_counters_mutex` | :white_check_mark: |

---

*レポート終了*
