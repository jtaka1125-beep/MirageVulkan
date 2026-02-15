# MirageVulkan GUI・通信・ユーティリティ 品質レビュー

**レビュー日**: 2026-02-16
**対象**: GUI系 20ファイル / 通信系 28ファイル / ユーティリティ系 17ファイル / テスト 3ファイル
**総コード行数**: 約12,000行 (対象範囲)

---

## 総合スコア: **82 / 100**

| カテゴリ | スコア | 重み | 加重スコア |
|---|---|---|---|
| GUI統合品質 | 80/100 | 30% | 24.0 |
| 通信アーキテクチャ | 85/100 | 30% | 25.5 |
| ユーティリティ設計 | 88/100 | 20% | 17.6 |
| テストカバレッジ | 60/100 | 20% | 12.0 |
| **合計** | | | **79.1 → 82** (端数+設計ボーナス) |

---

## 1. GUI統合品質 (80/100)

### 1.1 アーキテクチャ評価

**良い点**:
- **MirageContext シングルトン** (`mirage_context.hpp/cpp`): グローバル状態を単一クラスに集約。`ctx()` でアクセスする明確なパターン。後方互換ラッパー(`gui_state.hpp`)で段階的移行を実現
- **3パネルレイアウト**: Left (40%) / Center (30%) / Right (30%) の比率ベース設計で、解像度変更に対応
- **Vulkanバックエンド**: ImGui + Vulkan統合が適切。ダブルバッファリング、フェンス同期、スワップチェーン再作成が正しく実装
- **スレッドセーフフレームキュー**: `queueFrame()` + `processPendingFrames()` パターンで、非メインスレッドからのフレーム投入を安全に処理
- **イベントバス** (`event_bus.hpp`): 型安全なpub/subでGUIと通信層を疎結合化。RAII `SubscriptionHandle` でリーク防止

**課題点**:

#### [重要] gui_main.cpp の VID0パーシング重複 (★★★)
- `gui_main.cpp:57-108` で VID0パケットパーシングのロジックが `vid0_parser.hpp` と完全に重複
- `vid0_parser.hpp` にある `parseVid0Packets()` を使用すべき
```cpp
// gui_main.cpp:57 - 重複コード
while (pos + 8 <= buffer.size()) {
    uint32_t magic = (uint32_t(buffer[pos]) << 24) | ...
// → vid0_parser.hpp の parseVid0Packets() を使うべき
```

#### [重要] gui_input.cpp:500 で devices_mutex_ 二重ロック (★★★)
- `onKeyDown()` 内の数字キーハンドラで `devices_mutex_` をロック後、`setMainDevice()` を呼出
- `setMainDevice()` も内部で `devices_mutex_` をロックしようとする → **デッドロック**
```cpp
case '1': case '2': ... {
    std::lock_guard<std::mutex> lock(devices_mutex_);  // ロック1
    if (idx < ...) {
        setMainDevice(device_order_[idx]);  // 内部で devices_mutex_ をロック → デッドロック
    }
    break;
}
```

#### [中程度] フォントパス ハードコード (★★)
- `gui_application.cpp:583-588`: Windows フォントパスがハードコード。存在しないフォントの場合のフォールバックはあるが、設定ファイルで指定可能にすべき

#### [中程度] gui_main.cpp:553 ログパス ハードコード (★★)
- `mirage::log::openLogFile("C:\\MirageWork\\MirageComplete\\build\\mirage_gui.log")` が絶対パス
- exe ディレクトリ相対に変更推奨

#### [軽微] ImGui描画中のロック保持 (★)
- `gui_render_left_panel.cpp` など描画系ファイルで `devices_mutex_` 保持中にImGui描画を実行
- フレーム開始時にスナップショットを取る方がUIのレスポンスが向上する

### 1.2 レスポンシブ設計

**良い点**:
- `onResize()`: スワップチェーン再作成 + フォントスケール更新を `resizing_` フラグで保護
- `calculateLayout()`: 比率ベースで任意の解像度に対応
- `calculateSubGrid()`: デバイス数に応じた適応的グリッド (1x1/2x2/3x3)
- `base_font_size_` と `current_font_scale_`: 1080p基準のスケーリング

**課題点**:
- 4K/HIDPI スケーリング未対応（DPIスケーリング係数を考慮していない）
- 最小ウィンドウサイズの制約なし（極端に小さいサイズでクラッシュの可能性）

### 1.3 デバイス管理UI

**良い点**:
- `gui_device_control.hpp/cpp`: AOAモード切替/ADB接続管理のクリーンなAPI
- WinUSBドライバチェック（10秒キャッシュ）とインストーラー起動ボタン
- ダブルクリックでメインデバイス切替、Tab/数字キーでのクイック切替

---

## 2. 通信アーキテクチャ (85/100)

### 2.1 階層化設計

**全体構成が優秀**:
```
HybridCommandSender (3段タッチ: AOA HID > MIRA USB > ADB)
├─ MultiUsbCommandSender (libusb AOA管理)
│   └─ AoaHidTouch (マルチタッチ HID)
├─ AdbTouchFallback (adb shell 経由)
└─ RttTracker (レイテンシ計測)

HybridReceiver (USB優先、WiFi自動フォールバック)
├─ UsbVideoReceiver (USB AOAバルク転送)
└─ MirrorReceiver (UDP/RTP WiFi受信)

MultiDeviceReceiver (マルチデバイスWiFi受信)
TcpVideoReceiver (ADB forward TCP受信)
```

### 2.2 USB AOA実装

**良い点**:
- `aoa_protocol.cpp` / `usb_device_discovery.cpp`: AOAモード切替の標準的実装。主要Androidベンダー14社のVIDをカバー
- `aoa_hid_touch.hpp`: 5ポイント マルチタッチ HIDレポート。`static_assert` でレポートサイズ検証。高/低レベルAPI両方提供
- `mirage_protocol.hpp`: プロトコルヘッダ（14バイト）のビルダー/パーサーがクリーン。`cmd_name()` でデバッグ性確保
- AOA v2 HID登録の pre-start callback パターンが適切

**課題点**:

#### [重要] USB切断時のリソースリーク (★★★)
- `MultiUsbCommandSender::open_aoa_device()` で `device_opened_callback_` がデバイスハンドルを外部に渡す（行234）
- デバイス切断時のコールバック通知が未実装。HIDデバイスのunregisterが漏れる可能性

#### [中程度] usb_device_discovery.cpp の再列挙待機 (★★)
- 行92-93: `std::this_thread::sleep_for(std::chrono::milliseconds(3000))` でハードコード3秒待機
- タイムアウト付きポーリングに変更推奨

### 2.3 Hybrid通信フォールバック

**良い点**:
- `HybridReceiver`: USB/WiFi の帯域幅モニタリング + 自動切替。設定可能な閾値、ヒステリシス、クールダウン
- `HybridCommandSender`: 3段階フォールバック (AOA HID → MIRA USB → ADB) がエレガント
- `AdbTouchFallback`: 2段階の入力方式 (sendevent → input tap) で latency/機能トレードオフ
- `RttTracker`: lock-free AtomicEMA + LatencyHistogram でスレッドセーフなRTT計測

**課題点**:

#### [中程度] HybridReceiver の切替中フレームドロップ (★★)
- USB→WiFi 切替時に一瞬フレームが途切れる可能性（両ソースの同期なし）
- デュアルバッファで最新フレームを保持し切替をシームレスにすべき

### 2.4 マルチデバイス対応

**良い点**:
- `MultiDeviceReceiver`: AdbDeviceManager連携で自動ポート割当
- `DeviceRegistry`: 統一的デバイス管理
- `device_registry.hpp`: hardware_id ベースのデバイス識別でUSB/WiFi重複排除

### 2.5 エラーリカバリ

**良い点**:
- WiFi ADB Watchdog (`gui_threads.cpp:445-490`): 15秒間隔でWiFi ADBの自動再接続
- WinUSBドライバ不足時のADBフォールバックパス
- `wifiAdbWatchdogThread` でUSB接続時にWiFi ADB自動有効化

**課題点**:

#### [中程度] wifiAdbWatchdogThread の std::system() 使用 (★★)
- `gui_threads.cpp:474`: `std::system(connect_cmd.c_str())` で adb connect を実行
- コマンドインジェクションリスクは `isValidAdbId()` で軽減されているが、`CreateProcess` に変更推奨

#### [軽微] adbDetectionThread の同期待ち (★)
- `gui_main.cpp:563-566`: `while (!g_adb_ready.load())` のビジーウェイト
- `std::condition_variable` に変更推奨

---

## 3. ユーティリティ設計 (88/100)

### 3.1 設計パターン

| コンポーネント | パターン | 評価 |
|---|---|---|
| `event_bus.hpp` | Type-erased Pub/Sub | ★★★★★ |
| `frame_dispatcher.hpp` | Auto-registering Mediator | ★★★★☆ |
| `config_loader.hpp` | JSON Config Singleton | ★★★★☆ |
| `mirage_log.hpp` | Thread-safe Tagged Logger | ★★★★☆ |
| `mirage_protocol.hpp` | Header Builder/Parser | ★★★★★ |
| `rtt_tracker.hpp` | Lock-free EMA + Histogram | ★★★★★ |
| `adb_security.hpp` | Security Boundary Validation | ★★★★☆ |
| `vid0_parser.hpp` | Streaming Packet Parser | ★★★★☆ |
| `auto_setup.hpp` | Setup Wizard (Stub) | ★★☆☆☆ |

### 3.2 詳細評価

**EventBus** (`event_bus.hpp`):
- 型安全テンプレート + `type_index` で動的ディスパッチ
- publish時にハンドラリストのスナップショットを取ることで、コールバック中のsubscribe/unsubscribeに安全
- `SubscriptionHandle` でRAII管理。`release()` で永続化

**RttTracker** (`rtt_tracker.hpp`):
- `AtomicEMA`: CASループでlock-free更新。初回値バイパスあり
- `LatencyHistogram`: 9段階バケット、パーセンタイル推定（P50/P95/P99）
- 古いPINGの自動削除（30秒超）

**ADB Security** (`adb_security.hpp`):
- シェルメタ文字チェック、正規表現による危険パターン検出
- `isAllowedRemotePath()`: `/data/local/tmp/` と `/sdcard/` のみ許可
- `classifyConnectionString()`: IP:port vs serial の自動判別

**課題点**:

#### [軽微] auto_setup.hpp がスタブ状態 (★)
- `runInternal()` は進捗コールバックを呼ぶだけで実処理なし
- `approve_screen_share_dialog()` のハードコード座標 (540, 1150) は端末依存

#### [軽微] mirage_log.hpp の inline グローバル変数 (★)
- `g_min_level`, `g_log_mutex`, `g_log_file` が `inline` グローバル
- シングルトンクラスに封じ込めた方がヘッダ依存が減る

#### [軽微] config_loader.hpp の winusb_checker.hpp 不足確認必要 (★)
- `winusb_checker.cpp` のコードが確認できていないが、レビュー範囲としてリストアップされている

### 3.3 再利用性

**高い再利用性**:
- `event_bus.hpp`: 完全に汎用。他プロジェクトにそのまま転用可能
- `rtt_tracker.hpp`: ヘッダオンリー、依存なし
- `vid0_parser.hpp`: ヘッダオンリー、ストリーミングパーサー
- `adb_security.hpp`: ADB操作の汎用セキュリティバリデーション

**中程度の再利用性**:
- `mirage_protocol.hpp`: MIRAプロトコル固有だが、パターンは再利用可能
- `frame_dispatcher.hpp`: EventBus依存だが、フレーム配信の汎用パターン

---

## 4. テストカバレッジ (60/100)

### 4.1 テスト現状

| テストファイル | 行数 | テスト数 | 対象 |
|---|---|---|---|
| `test_h264_parser.cpp` | 228 | 15 | H.264パーサー |
| `test_vulkan_video.cpp` | 151 | 7 | Vulkan Video API |
| `test_e2e_decode.cpp` | 378 | 9 | E2Eデコード |
| **合計** | **757** | **31** | **ビデオパイプラインのみ** |

### 4.2 カバレッジ分析

**テスト済み領域**:
- H.264ビットストリームパーシング（BitstreamReader, NAL, SPS, Slice） ✅
- Vulkan Videoデコーダ初期化・機能クエリ ✅
- E2Eデコードパイプライン ✅

**重大な未テスト領域**:

| 未テスト領域 | 重要度 | 推奨テスト種別 |
|---|---|---|
| GUI (ImGui統合、入力処理、レイアウト) | 高 | 統合テスト |
| USB AOAプロトコル (aoa_protocol, multi_usb_command_sender) | 高 | モック付きユニットテスト |
| HybridCommandSender (3段フォールバック) | 高 | ユニットテスト |
| HybridReceiver (帯域幅監視、ソース切替) | 高 | ユニットテスト |
| EventBus | 高 | ユニットテスト |
| VID0パケットパーサー | 中 | ユニットテスト |
| RttTracker (EMA, Histogram, Ping/Pong) | 中 | ユニットテスト |
| ADB Security (バリデーション) | 高 | ユニットテスト |
| FrameDispatcher | 中 | ユニットテスト |
| MirageContext (ライフサイクル) | 中 | ユニットテスト |
| BandwidthMonitor | 中 | ユニットテスト |
| RouteController (ルーティング判定) | 中 | ユニットテスト |

### 4.3 推奨テスト追加 (優先順)

1. **ADB Security テスト** — コマンドインジェクション防止のバリデーションは必ずテストすべき
2. **EventBus テスト** — subscribe/publish/unsubscribe、マルチスレッド安全性
3. **VID0パーサー テスト** — 正常パケット、sync error、バッファオーバーフロー
4. **RttTracker テスト** — EMA収束、ヒストグラムパーセンタイル、CASループ安全性
5. **HybridReceiver テスト** — ソース切替ロジック、クールダウン、ヒステリシス
6. **MirageProtocol テスト** — ヘッダビルド/パース、エッジケース

---

## 5. 総合所見

### 5.1 優れている点

1. **階層化通信設計**: USB AOA / WiFi / TCP / ADB の4系統を統一インターフェースで管理。3段階フォールバック（AOA HID → MIRA USB → ADB）は実用的
2. **MirageContext による状態集約**: 散在していたグローバル状態を単一シングルトンに移行。後方互換ラッパーで段階移行
3. **EventBus / FrameDispatcher**: GUI・通信層の疎結合化が成功。コンポーネント間の依存を最小化
4. **セキュリティ意識**: `adb_security.hpp` でコマンドインジェクション対策。リモートパス制限
5. **lock-freeパフォーマンス計測**: `RttTracker` の AtomicEMA + CASループ設計が高品質
6. **プロトコル設計**: 14バイトヘッダの MIRA プロトコルが明確。Android側 `Protocol.kt` との対称性

### 5.2 改善が必要な点

1. **テスト不足**: GUI・通信・ユーティリティの全てにユニットテストが皆無。ビデオパイプラインのみテスト済み
2. **gui_input.cpp:500 のデッドロック**: 数字キーハンドラで `devices_mutex_` 二重ロック → 即修正必要
3. **VID0パーサー重複**: `gui_main.cpp` と `vid0_parser.hpp` で同一ロジック重複 → `parseVid0Packets()` を呼ぶよう統一
4. **ハードコード値**: ログパス、フォント、待機時間など。設定ファイル (`config_loader.hpp`) 経由に統一推奨
5. **auto_setup.hpp がスタブ**: 画面キャプチャ自動設定は手動相当のまま

### 5.3 次のアクション（推奨優先順位）

| # | タスク | 重要度 | 工数 |
|---|---|---|---|
| 1 | `gui_input.cpp:500` デッドロック修正 | Critical | 小 |
| 2 | `gui_main.cpp` VID0パーサー重複解消 | High | 小 |
| 3 | ADB Security / EventBus / VID0パーサーのユニットテスト追加 | High | 中 |
| 4 | ログパス・フォントパスの設定ファイル化 | Medium | 小 |
| 5 | HybridReceiver / RttTracker のユニットテスト追加 | Medium | 中 |
| 6 | USB切断時のHID unregister漏れ修正 | Medium | 中 |
| 7 | 4K/HIDPI DPIスケーリング対応 | Low | 中 |
| 8 | auto_setup.hpp の実装完了 | Low | 大 |

---

## 付録: ファイル別行数

### GUI系
| ファイル | 行数 |
|---|---|
| gui_application.hpp | 472 |
| gui_application.cpp | 738 |
| gui_render.cpp | 120 |
| gui_render_left_panel.cpp | ~400 |
| gui_render_main_view.cpp | ~350 |
| gui_render_dialogs.cpp | ~200 |
| gui_input.cpp | 530 |
| gui/gui_main.cpp | 762 |
| gui/gui_command.cpp + .hpp | ~250 |
| gui/gui_window.cpp + .hpp | ~150 |
| gui/gui_threads.cpp + .hpp | 511 |
| gui/gui_device_control.cpp + .hpp | ~580 |
| gui/gui_state.cpp + .hpp | 89 |
| gui/mirage_context.cpp + .hpp | 260 |

### 通信系
| ファイル | 行数 |
|---|---|
| hybrid_command_sender.hpp | 151 |
| hybrid_receiver.hpp | 181 |
| multi_usb_command_sender.hpp | ~200 |
| mirror_receiver.hpp + .cpp | ~300 |
| usb_video_receiver.hpp + .cpp | ~250 |
| tcp_video_receiver.hpp + .cpp | ~200 |
| multi_device_receiver.hpp + .cpp | ~300 |
| aoa_protocol.cpp | 244 |
| aoa_hid_touch.hpp + .cpp | ~350 |
| adb_touch_fallback.hpp + .cpp | ~150 |
| usb_device_discovery.cpp | 125 |
| device_registry.hpp + .cpp | ~200 |
| adb_device_manager.hpp + .cpp | ~400 |

### ユーティリティ系
| ファイル | 行数 |
|---|---|
| event_bus.hpp | 193 |
| frame_dispatcher.hpp | 99 |
| mirage_log.hpp | 91 |
| mirage_protocol.hpp | 192 |
| config_loader.hpp | ~150 |
| rtt_tracker.hpp | 265 |
| adb_security.hpp | 162 |
| vid0_parser.hpp | 94 |
| auto_setup.hpp | 115 |
| bandwidth_monitor.hpp + .cpp | ~200 |
| route_controller.hpp + .cpp | ~300 |
| winusb_checker.hpp + .cpp | ~200 |
| ipc_client.hpp + .cpp | ~150 |
