# MirageSystem 映像パイプライン仕様書

## 基本原則

### 解像度ポリシー: ネイティブ解像度厳守（変更禁止）

**Android端末のネイティブ解像度をエンコード→転送→デコード→表示の全工程で維持すること。**

- エンコーダ側（Android）: MediaProjectionが取得するVirtualDisplayはデバイスの物理解像度をそのまま使用する。`DisplayMetrics.widthPixels` x `heightPixels` をスケーリングなしで設定する
- 転送: 解像度に関する変換・リサンプリングは一切行わない
- デコーダ側（PC）: FFmpegデコーダの出力をそのまま使用する。sws_scaleによるリサイズ禁止
- 表示: GUI側でアスペクト比を維持した表示スケーリングのみ許可（ピクセルデータ自体は変更しない）

#### 禁止事項
- VirtualDisplay作成時の解像度を端末物理解像度から変更すること
- エンコーダのwidth/heightをネイティブ以外に設定すること
- デコード後のフレームにリサイズ処理を挟むこと
- 帯域節約目的での解像度ダウンスケーリング

#### 帯域制御の正しい方法
帯域が不足する場合は以下で対応する（解像度変更は使わない）:
- ビットレート調整（MediaCodec.setVideoBitrate）
- FPS調整（BandwidthMonitor/RouteControllerによる動的FPS制御: 60→30→15→10fps）
- I-frame間隔調整
- 品質パラメータ調整

### 確認済みデバイス解像度

| デバイス | Android | ネイティブ解像度 | 確認日 |
|---------|---------|----------------|--------|
| Npad X1 | 12 | 1080x1920 | 2026-02-11 |
| RebotAi A9 | 15 | 800x1268 | 2026-02-11 |

---

## パイプライン構成

### TCPモード（推奨）
```
Android                          PC
MediaProjection                  tcp_video_receiver
  → VirtualDisplay(native res)     ← ADB forward tcp:50100
  → MediaCodec H.264 encode        → RTP depayload
  → RTP packetize                   → H264Decoder (FFmpeg)
  → TcpVideoSender(:50100)         → RGBA frame
                                    → Vulkan texture → GUI
```

### UDPモード
```
Android                          PC
MediaProjection                  MultiDeviceReceiver
  → VirtualDisplay(native res)     ← UDP port 60000-60009
  → MediaCodec H.264 encode        → MirrorReceiver
  → RTP packetize                   → H264Decoder (FFmpeg)
  → UdpVideoSender                 → RGBA frame
                                    → Vulkan texture → GUI
```

### 通信設計
- 操作系 + 画像系の2系統をUSB + WiFiの2パイプで運用
- USB優先、帯域圧迫時は画像をWiFiに逃がし、WiFi不可ならFPS可変
- メイン: 60-30fps / サブ: 30-10fps
- 回線障害時は生存回線に全系統避難

---

## デコーダ仕様

### H264Decoder
- バックエンド優先順位: Vulkan → D3D11VA → CPU (swフォールバック)
- 出力フォーマット: RGBA (sws_scale NV12→RGBA変換のみ。リサイズなし)
- エラー -1094995529: SPS/PPS未受信のPフレーム。IDRフレーム到着で自動復旧

### SurfaceRepeater
- MediaTek SoC静止画面2fps問題の対策
- 最終フレームを定期的に再送信して安定したFPSを維持
