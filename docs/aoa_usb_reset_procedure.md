# AOA USB リセット手順書

**作成日:** 2026-03-11  
**対象デバイス:** Npad X1（VID=0E8D, MTK複合デバイス）  
**プロジェクト:** MirageVulkan / MirageSystem

---

## 背景・問題の概要

### MTKデバイス（Npad X1）のUSB PID構造

| モード | PID | sys.usb.config |
|--------|-----|----------------|
| ADB only | 0x2005 (RNDIS+ADB) | `adb` |
| ADB+AOA複合 | **0x201C** | `adb`（変わらない） |
| RNDIS+ADB | 0x2005 | `rndis,adb` |

> ⚠️ MTKデバイスは `sys.usb.config=adb` のままでも **PID=0x201C** で列挙される。  
> `sys.usb.config` の変化でAOAモード判定はできない。

### 発生した問題

1. GUIが起動時にPID=0x201Cを「AOAデバイス」と認識し直接open
2. Android側の `AccessoryIoService` が未起動 → EP_IN BULKエラー連発
3. `usb_device_discovery.cpp` に **PID=0x2005 を誤ってAOAリストに含めていた**バグ

---

## 根本原因と修正内容

### バグ：`usb_device_discovery.cpp` PIDリスト誤記

```cpp
// ❌ 修正前（バグあり）
uint16_t mtk_aoa_pids[] = {
    0x201C, // AOA composite (AOA+ADB)
    0x2005, // AOA only  ← RNDISなのでAOAではない
};

// ✅ 修正後
uint16_t mtk_aoa_pids[] = {
    0x201C, // AOA composite (AOA+ADB)
    // 0x2005 is RNDIS+ADB composite, NOT an AOA device
};
```

**修正ファイル:** `C:\MirageWork\MirageVulkan\src\usb_device_discovery.cpp` 行32

---

## AOA状態リセット手順（WiFi ADB経由）

### 前提条件

- WiFi ADB接続済み（`192.168.0.3:5555`）
- rootなし（一般アプリ権限）

### 手順1：AOAモードの検出

```powershell
# Android側のUSB設定を確認
adb -s 192.168.0.3:5555 shell getprop sys.usb.config
adb -s 192.168.0.3:5555 shell getprop sys.usb.state

# PC側のUSBデバイスPIDを確認
Get-PnpDevice | Where-Object {$_.HardwareID -match '0E8D'} |
  Select-Object Status,FriendlyName,InstanceId
```

**AOA状態のサイン:**
- PC側に `PID_201C` デバイスが `Status=OK` で存在
- Android側 `sys.usb.config` は `adb`（変化しないので参考程度）

### 手順2：USB functionをリセット（rootなし）

```bash
# 充電専用に切り替えてからADBに戻す
adb -s 192.168.0.3:5555 shell svc usb setFunctions
sleep 3
adb -s 192.168.0.3:5555 shell svc usb setFunctions adb
sleep 3

# 確認
adb -s 192.168.0.3:5555 shell getprop sys.usb.config
# → "adb" に戻っていればOK
```

> **注意:** `setprop sys.usb.config` はSELinuxで拒否される（rootなし環境では不可）。  
> `svc usb setFunctions` を使うこと。

### 手順3：RNDIS復活が必要な場合

RNDISが消えた場合（AOA操作後など）：

```bash
# RNDIS+ADB構成に戻す（要root or 開発者オプション）
adb -s 192.168.0.3:5555 shell svc usb setFunctions rndis,adb
sleep 3

# 確認
adb -s 192.168.0.3:5555 shell ip addr show usb0
# RNDISのIPが復活するはず（例: 10.178.141.11）
```

### 手順4：PC側のゴーストデバイス無効化

WinUSBドライバが残存デバイスを掴んでいる場合：

```powershell
# 不要なPID=201CデバイスをdevconでDisable
C:\MirageWork\tools\devcon.exe disable "@USB\VID_0E8D&PID_201C\93020523431940"

# 確認
C:\MirageWork\tools\devcon.exe status "@USB\VID_0E8D&PID_201C\93020523431940"
```

> ⚠️ 実際に物理接続中のPID=201Cを誤ってdisableすると接続が切れる。  
> 事前に `pnputil /enum-devices /bus USB /connected` で接続中かどうか確認すること。

---

## AOA接続フロー（正常）

```
[PC GUI起動]
    │
    ├─ PID=0x201C 検出（MTK複合）→ WinUSBでopen
    │       ↓
    │   EP_OUT送信 → Android側 HIDデバイス作成（/dev/input/eventX）
    │       ↓
    │   [Android側] AccessoryIoService が /dev/usb_accessory をopen
    │       ↓
    │   EP_IN受信開始 → ACK通信確立
    │
    └─ 成功: "USB AOA mode (1 device(s)) - dual-channel active"
```

### AccessoryIoServiceを手動起動する方法

```bash
# GUI起動直後にADB経由で強制起動
adb -s 192.168.0.3:5555 shell am startservice \
  -n com.mirage.capture/.usb.AccessoryIoService

# またはActivityを起動（1秒ごとに自動検出→起動）
adb -s 192.168.0.3:5555 shell am start \
  -n com.mirage.capture/.ui.AccessoryActivity
```

---

## トラブルシューティング

### LIBUSB_ERROR_IO / LIBUSB_ERROR_PIPE が連発する

| 症状 | 原因 | 対処 |
|------|------|------|
| EP_IN PIPE #1 → IO #2〜5 | Android側がaccessoryを開いていない | AccessoryIoService起動 |
| IO継続・ACK timeout | WinUSBドライバ不整合 | devcon disable→再接続 |
| LIBUSB_ERROR_NOT_SUPPORTED | WinUSBドライバ未インストール | GUI [Driver Setup] or install_android_winusb.py |
| LIBUSB_ERROR_ACCESS | OS handle leak（前プロセスのゾンビ） | USB抜き差し or PC再起動 |

### Android側 `accessoryList is empty` になる

```
MirageAccessoryIO: UsbManager.accessoryList is empty
```

**原因:** USB_ACCESSORY_ATTACHED ブロードキャストが届かなかった

**対処:**
1. GUI起動中にUSBを抜き差し
2. または上記の `am start AccessoryActivity` を実行

### RNDIS（usb0）インターフェースが消える

USB操作後にRNDISが消える場合：

```bash
# 確認
adb -s 192.168.0.3:5555 shell ip addr | grep -E 'usb|rndis'

# 復旧
adb -s 192.168.0.3:5555 shell svc usb setFunctions rndis,adb
```

---

## デバイス構成メモ

| デバイス | WiFi IP | USB Serial | AOA PID |
|---------|---------|-----------|---------|
| Npad X1 | 192.168.0.3:5555 | 93020523431940 | 0x201C |
| A9 #956 | 192.168.0.6:5555 | A9250700956 | 0x201C |
| A9 #479 | 192.168.0.8:5555 | A9250700479 | 0x201C |

---

## 関連ファイル

| ファイル | 用途 |
|---------|------|
| `src/usb_device_discovery.cpp` | AOA PIDリスト・switch処理 |
| `src/aoa_protocol.cpp` | open_aoa_device・switch_device_to_aoa_mode |
| `src/multi_usb_command_sender.cpp` | EP_INエラーハンドリング |
| `android/.../AccessoryIoService.kt` | Android側accessory open |
| `android/.../AccessoryActivity.kt` | 1秒ポーリング・Permission要求 |
| `tools/devcon.exe` | WinUSBデバイス管理 |
| `install_android_winusb.py` | WinUSBドライバ一括インストール |
