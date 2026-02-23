# TCP Mirror 映像非表示 診断レポート

## 結論

**映像が表示されない原因は2つの映像パイプラインの競合**である。

MultiDeviceReceiver (WiFi/UDP) と TcpVideoReceiver (ADB Forward/TCP) の両方が同時に起動しているが、
Android側の ScreenCaptureService は **WiFi UDP モードで起動** されており、映像を `192.168.0.7:61000-61002` に UDP 送信している。
TcpVideoReceiver 側はポート 50100-50102 で TCP 接続しているが、Android アプリは TCP 側にはデータを送っていない。

---

## 詳細分析

### 1. 起動シーケンス（ログから判明）

```
gui_main.cpp:710  initializeMultiReceiver()   → 成功 (UDP ポート 61000-61002)
gui_main.cpp:711  initializeHybridCommand()   → USB AOA 0台 (WinUSB未インストール)
gui_main.cpp:712  initializeUsbAoaVideo()     → AOA デバイスなし
gui_main.cpp:713  initializeTcpReceiver()     → 成功 (TCP ポート 50100-50102)
```

### 2. initializeMultiReceiver() の動作

- `MultiDeviceReceiver::start(61000)` で各デバイスに UDP リスナーを起動
- `startScreenCaptureOnAll("192.168.0.7", 61000)` を呼び出し
- Android 側で `am start ... --es mirror_host "192.168.0.7" --ei mirror_port 61000` を実行
- **Android アプリは UDP で `192.168.0.7:61000` に映像送信開始**

### 3. initializeTcpReceiver() の動作

- `TcpVideoReceiver::start(50100)` で各デバイスに TCP スレッドを起動
- 各デバイスに `adb forward tcp:5010X tcp:50100` を設定
- `127.0.0.1:5010X` に TCP 接続を試行
- TCP 接続自体は成功するが、**Android アプリは TCP 50100 でリッスンしていない**
  （ScreenCaptureService は UDP 送信モードで動作中）

### 4. ログの証拠

```
11:44:22.585 [tcpvideo] Connected to 6810568f_37627893 via TCP port 50100   ← TCP接続成功
11:50:30.170 [tcpvideo] No data from 6810568f_37627893, backoff 4000ms      ← データ来ない
```

TCP 接続は確立できる（adb forward が Android のポート 50100 に転送）が、
Android の ScreenCaptureService は UDP 送信モードのため、TCP ポート 50100 にはデータがない。

### 5. WiFi UDP 側でも映像が来ない理由

ログ全体で `WiFi=0.0Mbps(alive=0)` が持続しており、MultiDeviceReceiver にもデータが来ていない。

**考えられる原因：**
- Windows ファイアウォールが UDP ポート 61000-61002 の受信をブロック
- Android 端末と PC が**同じサブネット**だが、UDP パケットがルーティングされていない
- 全デバイスが WiFi 接続（USB:0）のため、WiFi 経由の UDP は PC の WiFi NIC に到達する必要がある
- `192.168.0.7` が PC の正しい IP かの確認が必要

### 6. `tcp_recv_test.exe` で成功した理由

ユーザーは別途 `adb forward tcp:50100 tcp:50100` を手動設定し、
`tcp_recv_test.exe` で `127.0.0.1:50100` に接続して映像デコードに成功した。

これは **ScreenCaptureService が TCP サーバーモードで稼働していたデバイス** に対して動作した可能性が高い。
あるいは、Android アプリの ScreenCaptureService が TCP リスナーも並行して持っている場合、
GUIの initializeMultiReceiver が先に UDP モードで起動してしまい、TCP リスナーが無効になった可能性がある。

---

## 問題の根本原因

| 問題 | 詳細 |
|------|------|
| **WiFi UDP パイプライン** | PC のファイアウォールまたはネットワーク設定により UDP 受信が失敗 |
| **TCP ADB Forward パイプライン** | Android アプリが UDP 送信モードで起動済みのため、TCP ポートにデータなし |
| **二重起動** | MultiDeviceReceiver と TcpVideoReceiver が同時に稼働し、リソースを浪費 |

---

## 修正案

### 即座の対処（映像を表示するために）

#### 方法A: Windows ファイアウォールで UDP 許可

```powershell
# 管理者権限で実行
netsh advfirewall firewall add rule name="MirageVideo UDP" dir=in action=allow protocol=UDP localport=61000-61010
```

その後 GUI を再起動し、MultiDeviceReceiver (UDP) 経由で映像が来るか確認。

#### 方法B: TCP モードのみ使用（MultiDeviceReceiver を無効化）

1. `initializeMultiReceiver()` を呼ばないようにする
2. Android アプリを TCP リスナーモードで起動
3. TcpVideoReceiver のみで受信

### 自動開始の改善案

#### 案1: フォールバック方式（推奨）

```
gui_main.cpp の起動シーケンスを変更:

1. initializeMultiReceiver() を試行
2. 5秒以内にフレームが来なければ → TcpVideoReceiver にフォールバック
3. TCP でも来なければ → ユーザーに通知
```

#### 案2: 設定ベースの選択

`config.json` に `video.mode` を追加:

```json
{
  "video": {
    "mode": "auto",        // "auto" | "udp" | "tcp"
    "default_route": "usb"
  }
}
```

- `auto`: UDP を試行 → 失敗時 TCP
- `udp`: MultiDeviceReceiver のみ
- `tcp`: TcpVideoReceiver のみ

#### 案3: 排他制御

`initializeMultiReceiver()` が成功したら `initializeTcpReceiver()` をスキップ：

```cpp
// gui_main.cpp:710-714
bool multi_recv_active = initializeMultiReceiver();
// ...
if (!multi_recv_active) {  // ← 追加: UDP 失敗時のみ TCP を使う
    bool tcp_recv_active = initializeTcpReceiver();
    MLOG_INFO("gui", "TCP receiver: %s", tcp_recv_active ? "active" : "inactive");
}
```

---

## 「TCP MIRROR START」ボタンについて

GUIのコードに「TCP MIRROR START (ADB FORWARD)」ボタンは**存在しない**。
Device Control パネルには以下のボタンのみ：
- All Devices AOA Mode
- ADB Connect / ADB(USB) / ADB(WiFi)
- AOA (個別デバイス)
- WinUSB Driver Install

ScreenCaptureService の再起動は Left Panel の startMirroringCallback 経由で可能だが、
これは WiFi UDP モードで起動するため、TCP パイプラインには影響しない。

---

## 推奨 デバッグ手順

1. **ファイアウォール確認**
   ```
   netsh advfirewall firewall show rule name=all | findstr /i "mirage\|61000"
   ```

2. **PC の IP 確認**
   ```
   ipconfig | findstr /i "192.168"
   ```
   `192.168.0.7` が正しい WiFi アダプタの IP か確認

3. **UDP 受信テスト**
   ```
   # Pythonで確認
   python -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('0.0.0.0',61000)); print(s.recvfrom(1024))"
   ```

4. **Android 側の送信確認**
   ```
   adb -s 192.168.0.6:5555 shell dumpsys activity services com.mirage.android
   ```
