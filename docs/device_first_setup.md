# MirageSystem デバイス初期セットアップ仕様書

## 概要

MirageSystemで新しいAndroidデバイスを運用するには、**初回接続時（ファーストインパクト）にUSB経由で必須設定を完了する**必要がある。  
USB接続が「充電のみ」になった場合、PC側からの自動復帰は不可能なため、予防的な設定が最重要。

## 前提条件

- PC: Windows 10/11、ReTRY HUB (CT-USB4HUBV2) 接続済み
- Android: USB デバッグ有効、ADB認証済み

---

## ファーストインパクト（初回USB接続時の必須手順）

### 1. USB転送モード設定（最優先）

充電のみモードからのPC側自動復帰は**不可能**。初回接続時に必ずデフォルトを変更する。

```bash
# PTPをデフォルトに設定（永続）
adb -s <SERIAL> shell settings put global usb_configuration ptp

# 確認
adb -s <SERIAL> shell getprop sys.usb.state
# 期待値: ptp,adb
```

**なぜPTPか：**
- MTP: ファイル転送モード。一部機種で不安定
- PTP: カメラモード。軽量で安定、AOA切り替えとの相性が良い
- 充電のみ: ADB接続不可。PC側からの復帰手段なし

### 2. Wi-Fi ADB設定

USB接続が切れた場合のフォールバック経路として必須。

```bash
# Wi-Fi ADB有効化（USB接続中に実行）
adb -s <SERIAL> tcpip 5555

# IPアドレス確認
adb -s <SERIAL> shell ip route | grep wlan0
# → 192.168.0.0/24 dev wlan0 ... src 192.168.0.X

# Wi-Fi経由で接続確認
adb connect 192.168.0.X:5555
```

**Wi-Fi ADBが必要な理由：**
- USB HUBのError復帰時にUSB ADBが一時的に使えなくなる
- パワーサイクル後のデバイス制御に必要
- Bluetoothペアリングの補助経路

### 3. Bluetoothペアリング

Wi-Fiが切れた場合の最終フォールバック経路。

#### Android側の準備（ADB経由で自動化可能）

```bash
# BT有効化
adb -s <SERIAL> shell svc bluetooth enable

# BT MACアドレス取得
adb -s <SERIAL> shell settings get secure bluetooth_address
# → XX:XX:XX:XX:XX:XX

# Discoverable有効化（300秒）
adb -s <SERIAL> shell am start -a android.bluetooth.adapter.action.REQUEST_DISCOVERABLE \
  --ei android.bluetooth.adapter.extra.DISCOVERABLE_DURATION 300

# 「許可」ダイアログの自動タップ（UIAutomator経由）
# button1 = 許可, button2 = 許可しない
# ※TAB+ENTERは「許可しない」にフォーカスが当たるのでNG
# UIAutomatorでtext="許可"のboundsを取得してinput tapする
```

#### PC側のペアリング（Windows設定UI経由が必須）

**重要: WinRT `PairAsync` はDesktopアプリから `AccessDenied` になる。**

検証済みの方法:
- `FindAllAsync(PairingState=false)` → 未ペアリングデバイスが見つからない（キャッシュ問題）
- `FromBluetoothAddressAsync(MAC)` → デバイス検出OK、`CanPair:True`
- `PairAsync()` / `Custom.PairAsync()` → **AccessDenied**（PowerShell/C# EXE両方）
- Windows設定UI「デバイスの追加」→「Bluetooth」→ デバイス選択 → **成功**

**唯一の確実な方法: Windows設定UIの「デバイスの追加」ダイアログ経由**

```
手順:
1. ms-settings:connecteddevices を開く
2. 「デバイスの追加」ボタンをクリック
3. 「Bluetooth」を選択
4. デバイス一覧からA9を選択
5. PIN確認で「接続」をクリック
6. Android側で「ペア設定する」をタップ
```

#### 自動化の方針

| 工程 | 自動化方法 | 状態 |
|------|-----------|------|
| Android BT有効化 | ADB `svc bluetooth enable` | ✅ 可能 |
| Android Discoverable | ADB + UIAutomator tap | ✅ 可能（座標タップ） |
| PC デバイス追加UI | UIAutomation / SendKeys | 🔧 要実装 |
| PC PIN確認「接続」 | UIAutomation / SendKeys | 🔧 要実装 |
| Android ペア承認 | UIAutomator tap | ✅ 可能 |

### 4. 開発者オプション確認

```bash
# スリープしない（充電中）
adb -s <SERIAL> shell settings put global stay_on_while_plugged_in 3

# USB デバッグ認証の永続化
adb -s <SERIAL> shell settings put global adb_wifi_enabled 1
```

---

## 接続経路の優先順位

| 優先度 | 経路 | 用途 | 障害時 |
|--------|------|------|--------|
| 1 | USB (AOA/WinUSB) | 画面ミラーリング・操作 | HUBポートcycleで復帰 |
| 2 | USB ADB | コマンド実行・設定変更 | PTP設定で予防 |
| 3 | Wi-Fi ADB | USBが使えない時のフォールバック | reconnectで復帰 |
| 4 | Bluetooth PAN ADB | Wi-Fiも切れた時の最終手段 | 要ペアリング済み |

---

## トラブルシューティング

### 充電のみになった場合

**PC側からの自動復帰は不可能。** 以下のいずれかが必要：

1. **端末の画面操作**で USBモードをPTP/MTPに切り替え
2. **Wi-Fi ADB経由**で `adb shell svc usb setFunctions ptp` を実行
3. **Bluetooth PAN ADB経由**で同上

→ だからファーストインパクト時にWi-Fi/BTを設定しておく

### ReTRY HUB が認識されない（Hubs found: 0）

```bash
# HUBデバイスの状態確認
powershell -Command "Get-PnpDevice -Class USB | Where-Object { $_.InstanceId -like '*0424*2744*' }"

# Error状態の場合 → 再有効化
powershell -Command "Enable-PnpDevice -InstanceId 'USB\VID_0424&PID_2744\MSFT202533' -Confirm:$false"

# それでもダメ → HUBのUSBケーブルをPC側で抜き差し
```

### HUBポートのデバイスが消えた

```bash
# 全ポートON
retryhub_ctrl.exe on all

# ステータス確認
retryhub_ctrl.exe status
```

### Wi-Fi ADB が offline

```bash
adb disconnect <IP>:5555
adb connect <IP>:5555
```

---

## MCPツール（MirageServer v4.4.0）

### usb_hub_control

ReTRY HUBポートの電源制御。

```
action: status | on | off | cycle
port: 1-4 | all
delay_ms: cycleの待機時間（デフォルト3000ms）
```

### usb_recovery

USB完全リカバリ（パワーサイクル→再接続待ち→充電のみ対策）。

```
port: HUBポート番号（1-4）
device: ADBシリアル（省略可）
delay_ms: パワーサイクル待機時間
wait_timeout: デバイス再認識タイムアウト（秒）
```

**注意**: 充電のみ状態からの復帰はADB経由（Wi-Fi/BT）が必要。USB側のみでの復帰は不可。

---

## ビルド済みツール

| ツール | パス | 用途 |
|--------|------|------|
| retryhub_ctrl.exe | tools/retryhub_ctrl.exe | USB HUBポート電源制御 |
| bt_pair.exe | tools/bt_pair.exe | BTペアリング（C# WinRT, AccessDenied制限あり） |

### bt_pair.exe の制限

`FromBluetoothAddressAsync` でデバイス検出→`Custom.PairAsync` でペアリング試行するが、
Desktop appからの実行では `AccessDenied` になる（Windows仕様）。
デバイス検出の確認用途には使える:

```bash
bt_pair.exe 9C:92:53:F4:CA:8F
# → Found: A9, CanPair:True, IsPaired:True/False
```

---

## 初期セットアップ自動化スクリプト

`auto_setup/bluetooth_adb_setup.py` の `auto_pair()` メソッドで
Android側（Discoverable + ダイアログ承認）は自動化済み。

PC側のUI自動化は未実装（UIAutomation APIで `ms-settings:connecteddevices` の操作が必要）。

---

## 変更履歴

| 日付 | 内容 |
|------|------|
| 2026-02-15 | 初版作成。充電のみ問題の検証結果を反映 |
| 2026-02-15 | BTペアリング検証結果追加。WinRT PairAsync AccessDenied問題を記録 |
