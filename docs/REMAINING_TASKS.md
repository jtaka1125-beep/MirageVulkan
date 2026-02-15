# MirageSystem v2 残タスク指示書
**作成日**: 2026-02-07
**最終更新**: 2026-02-11
**対象**: C:\MirageWork\MirageComplete

---

## 完了タスク一覧 (2026-02-07 全完了)

| # | タスク | 状態 | 備考 |
|---|--------|------|------|
| 1 | ファイルロック実装 | ✅ 完了 | 既に実装済みと確認 |
| 2 | connection_daemon ADB統合 | ✅ 完了 | tray_app↔connection_daemon API既に整合 |
| 3 | テストカバレッジ強化 | ✅ 完了 | 5テストファイル作成 |
| 4 | ドキュメント整備 | ✅ 完了 | AI_ARCHITECTURE.md, CODE_AUDIT_REPORT.md 作成 |
| 5 | バッテリー情報表示 | 🔶 未着手 | 低優先度、GUIに表示追加 |
| 6 | requirements.txt 作成 | ✅ 完了 | ルート+auto_setup+scripts+driver_installer |
| 7 | __init__.py 追加 | ✅ 完了 | auto_setup+driver_installer |
| 8 | ゾンビプロセス対策 | ✅ 完了 | scripts/ 4ファイル修正 |
| 9 | IP/ポート設定統一 | ✅ 完了 | config.json + config_loader更新 |
| 10 | ログローテーション上限 | ✅ 完了 | 既に設定済み確認 |
| 11 | device_init_macro 未定義参照 | ✅ 完了 | 既に解決済み |
| 12 | tray_app PyQt5未インストール時クラッシュ | ✅ 完了 | スタブ追加 |

---

## 設計前提 (重要)

**USBデバッグなしで動作させることが前提**

- **本番メイン経路**: USB AOA (映像受信 + コマンド送信)
  - USBデバッグ不要、AOAモードのみ
  - セキュリティリスク最小
- **本番サブ経路**: Wi-Fi (RTP映像 + UDPコマンド)
  - USBデバッグ不要
- **フォールバック/デバッグ**: ADB
  - AOA不具合時の映像取得 (screencap)
  - AOA操作が効かない時のコマンド送信 (adb shell input)
  - 初期セットアップ (Wi-Fi ADB, BT PAN設定)
  - トラブルシューティング

理由: ネット常時接続端末でUSBデバッグONはセキュリティリスク大
(ADBポート5555経由の不正アクセス、APKインストール等)

---

## 通信設計

- 操作系+画像系の2系統をUSB+WiFiの2パイプで運用
- USB優先、帯域圧迫→画像WiFi逃がし→WiFi不可ならFPS可変
- メイン60-30fps / サブ30-10fps
- 回線障害時は生存回線に全系統避難

---

## 2026-02-11 追加: Android APK 2分割設計

### 決定事項
Android側アプリを2つのAPKに分離する。

### APK構成

**1. MirageCapture (com.mirage.capture)**
- 役割: 画面キャプチャ＋映像送信専用
- 機能:
  - MediaProjection → H.264ハードウェアエンコード
  - WiFi経由: RTP/UDP送信
  - USB経由: AOA Bulk転送 (将来)
  - Foreground Serviceで常駐
- 許可ダイアログ: 「画面の録画を開始しますか？」(MediaProjection) → 初回1回
- 特徴: AOA接続状態に依存しない。AOAが切れても映像送信を継続可能
- Android要件: `foregroundServiceType="mediaProjection"` (Android 14+対応)

**2. MirageAccessory (com.mirage.accessory)**
- 役割: AOAアクセサリ接続＋コマンド受信専用
- 機能:
  - USB Accessory接続ハンドリング
  - MIRAプロトコル コマンド受信 → タップ/スワイプ/キー実行
  - AccessibilityServiceまたはinput injection
- 許可ダイアログ: 「このUSBアクセサリへのアクセスを許可しますか？」→ 初回1回
- Manifest設定:
  - `USB_ACCESSORY_ATTACHED` intent-filter
  - `res/xml/accessory_filter.xml` (manufacturer="Mirage", model="MirageSystem")
  - `android:directBootAware="true"` (リブート後も許可永続)

### 分離のメリット
- **障害分離**: AOA切断時もキャプチャ継続、キャプチャ停止時もAOAコマンド継続
- **ライフサイクル独立**: USB再接続でAccessory側だけ再起動、Capture側は無影響
- **フォールバック安定**: ADBコマンドモード時もCapture側は正常動作
- **更新容易**: AOAプロトコル変更時にCapture側の更新不要、逆も同様
- **設計思想と一致**: 2系統(操作+画像)×2パイプ(USB+WiFi)の独立性を保証

### インストール
- MirageAutoSetupが `adb install` で2つとも自動インストール
- ユーザー手間: 許可ダイアログ2回のみ (両方とも自動化対応可能)

---

## 2026-02-11 追加: AOA許可ダイアログ自動化方針

### 問題
AOAモード切替時にAndroidがシステムダイアログ「このUSBアクセサリへのアクセスを許可しますか？」を表示する。初心者がこれを手動で承認する必要がある。

### 解決方針 (3段構え)

**方針1: Android側 intent-filter + device_filter.xml (最優先)**
- MirageAccessoryのAndroidManifest.xmlに `USB_ACCESSORY_ATTACHED` intent-filter追加
- `res/xml/accessory_filter.xml` にMirage AOA識別情報を記載
- `android:directBootAware="true"` 設定でリブート後も許可永続
- 効果: 初回のみダイアログ表示、「常にこのアプリで開く」チェックで2回目以降は自動許可
- Google公式仕様として「最低1回の確認は必須」(Won't Fix扱い)

**方針2: 初回ダイアログをADB uiautomator dumpで自動タップ (フォールバック)**
- `adb shell uiautomator dump` でUI要素のbounds取得
- 「許可」ボタンと「常にこのアプリで開く」チェックボックスの座標を特定
- `adb shell input tap x y` で自動タップ
- MirageAutoSetup (PC側Python) から実行

**方針3: MirageGUI自身のミラーリング+マクロ機能で自動承認 (保険)**
- セットアップフロー:
  1. USB ADB接続 (初期状態)
  2. Wi-Fi ADB自動有効化 (`adb tcpip` + `adb connect`)
  3. Wi-Fi経由でMirageCaptureがスクリーンキャプチャ開始
  4. PC側からAOAモード切替実行
  5. ダイアログ出現 → Wi-Fiミラーリングで画面が見えている
  6. OCR/テンプレートマッチングで「許可」ボタン検出 → 自動タップ送信
- 自身のマクロ機能を活用するため外部ツール不要
- MirageCaptureが独立しているのでAOA切替の影響を受けない ← 2分割の利点

### Wi-Fi ADB自動有効化 (問題なし)
- USB ADB接続状態から:
  1. `adb -s {serial} shell ip route` でIPアドレス取得
  2. `adb -s {serial} tcpip 5555` でWi-Fi ADB有効化
  3. `adb connect {ip}:5555` でWi-Fi ADB接続
- ⚠️ Reboot時はWi-Fi ADBがOFFになるため自動再有効化が必須

### 実装順序
1. まずMirageAccessoryのManifest/device_filter.xml修正 (方針1)
2. MirageAutoSetupにuiautomator自動タップ追加 (方針2)
3. MirageGUIマクロに許可ダイアログ自動承認フロー追加 (方針3)

---

## 2026-02-11 追加: AOAドライバ問題と暫定対応

### 問題
Windows標準のAndroid ADB Interfaceドライバが有効な状態では、libusb経由でUSBデバイスを開けない (LIBUSB_ERROR_ACCESS)。AOAモードのデバイス (18D1:2D01) も同様。

### 暫定対応 (実装済み)
- `gui_main.cpp`: USB 0台でもルーティング初期化 (WiFi-onlyモード対応)
- `gui_main.cpp`: ADBフォールバックモード表示
- `gui_device_control.cpp`: WinUSBドライバ未インストール警告+インストールボタン
- `aoa_protocol.cpp`: ACCESS/NOT_SUPPORTED時のエラーメッセージ改善

### 本格対応 (TODO)
- WinUSBドライバ自動インストール (AOAインターフェース用)
- または ADB forward によるTCPトンネル方式

---

## 残作業 (低優先度)

### バッテリー情報表示
- `AdbDeviceManager` に `getBatteryLevel` 追加
- GUI左パネルのデバイスリストに `[78%]` 表示
- ADBフォールバック時のみ使用 (AOA経由でも取得方法検討)

### WinUSBドライバ自動インストール
- AOA本番運用に必要
- driver_installer/ に仕組みあり (要テスト)

### マクロエディタ Phase 2
- pywebview ブリッジタイミング問題修正
- 実機ADB実行モード (mock→real切替)
- テンプレートマッチング連携ブロック追加
