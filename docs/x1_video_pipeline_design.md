# X1 Video Pipeline — Zero-Base Design v2
Date: 2026-03-07  Rev: 2

## 物理的前提（変更不可）

| 制約 | 原因 |
|---|---|
| HWエンコーダは1200×2000を拒否 | MTK PerformancePoint(1920×1088@30) 上限 |
| VirtualDisplay出力上限 ~30fps | MTK Dimensity 1100 SoC制限 |
| USBデバッグ不要 | ADB/adb reverse/scrcpy前提を完全排除 |

---

## 設計原則（v1からの変更）

**v1「Mode A/B 切替」は廃止。理由：切替時に座標系が揺れ、AI/マクロ判定が壊れる。**

代わりに **2レーン並走** を採用：

| Lane | 目的 | 解像度 | FPS | コーデック | 常時稼働 |
|---|---|---|---|---|---|
| **Canonical** | AI/OCR/マクロ/座標基準 | 1200×2000 | 30 | JPEG | ✅ 常時 |
| **Presentation** | 人間向け表示 | 1920×1080 | 60 | H.264 HW | オプション |

**絶対ルール：**
- AI/マクロへの入力は Canonical Lane のみ
- Presentation Lane のフレームは AI に流さない
- 全操作座標は 1200×2000 基準で統一（Presentation 表示中でも変えない）
- Canonical Lane は補間・合成・平滑化なし（原画像そのまま）

---

## アーキテクチャ

```
[Android X1]

ProjectionSession (MediaProjection × 1)
  │
  ├── VirtualDisplay #1 ─── 常時稼働
  │     │   1200×2000 / RGBA_8888 / maxImages=3
  │     ▼
  │   ImageReader
  │     │  Choreographer VSync駆動 (30fps throttle)
  │     ▼
  │   JpegEncoder (3スレッド, ThreadLocal<Bitmap>+BAOS)
  │     │  補間なし / 色変換なし / 原画像そのまま
  │     ▼
  │   UdpFrameSender ─► UDP :50201  [Canonical Lane]
  │
  └── VirtualDisplay #2 ─── PRES_ON時のみ存在
        │   1920×1080
        ▼
      SurfaceRepeater (EGL pacing, 60fps強制)
        ▼
      MediaCodec H.264 HW
        │  bitrate可変 / キーフレーム制御
        ▼
      UdpFrameSender ─► UDP :50202  [Presentation Lane]

TcpControlServer :50200
  ← HELLO / CAPS / START / IDR / PRES_ON / PRES_OFF / PING / BYE

─────────────────────────────────────────────────

[PC MirageVulkan]

X1SessionManager
  ├── TcpControlClient :50200  (keepalive / IDR / PRES_ON/OFF)
  │
  ├── UdpVideoReceiver :50201  ← Canonical Lane
  │     │  フラグメント再構築 / frame_id管理 / 古フレーム破棄
  │     ▼
  │   CanonicalDecoder
  │     │  stb_image JPEG → RGBA8
  │     │  DX11 upload → ID3D11Texture2D
  │     ▼
  │   CanonicalFrame (1200×2000, frame_id, pts_us)
  │     ├──► AI Engine (OCR / テンプレート / マクロ)
  │     ├──► 座標系基準 (全タップ/スワイプはここ基準)
  │     └──► 表示 (Presentation Laneが無い時)
  │
  └── UdpVideoReceiver :50202  ← Presentation Lane (オプション)
        │  (PRES_ONコマンド送信後に受信開始)
        ▼
      PresentationDecoder
        │  MF H.264 HW decode → D3D11 NV12 texture
        ▼
      PresentationFrame (1920×1080)
        └──► 表示専用 (AI/マクロには渡さない)
```

---

## ネットワークプロトコル

### ポート割当

| ポート | 用途 | プロトコル |
|---|---|---|
| 50200 | 制御 | TCP |
| 50201 | Canonical映像 | UDP |
| 50202 | Presentation映像 | UDP |

### 制御コマンド (TCP :50200)

```
PC → X1:  HELLO
X1 → PC:  CAPS canonical=1200x2000@30 presentation=1920x1080@60
PC → X1:  START
X1 → PC:  OK
          [Canonical UDP開始]
PC → X1:  PRES_ON          # Presentation Lane開始要求
X1 → PC:  OK
          [Presentation UDP開始]
PC → X1:  PRES_OFF         # Presentation Lane停止
PC → X1:  IDR              # Canonical キーフレーム要求
PC → X1:  IDR_PRES         # Presentation キーフレーム要求
PC → X1:  BITRATE kbps=N   # Presentation ビットレート変更
PC → X1:  PING
X1 → PC:  PONG
PC → X1:  BYE
```

### 映像フレームヘッダ (UDP, 24 bytes big-endian)

```
offset  size  field
0       4     magic    "MFRM" (0x4D46524D)
4       1     lane     0x01=Canonical  0x02=Presentation
5       1     flags    bit0=keyframe  bit1=first_frag  bit2=last_frag
6       2     frag_idx フラグメントindex (0-based)
8       4     frame_id
12      8     pts_us
20      2     width
22      2     height
─────────────── 24 bytes ───────────────
[payload: JPEG全体 or H264 NALユニット]
```

**フラグメント:**
- MTU payload = 1400 bytes
- 1フレームが1400超 → 複数datagramに分割
- `last_frag` ビットで末尾判定
- PC側: frame_id単位でバッファ管理

---

## VideoFrame型

```cpp
// src/video/video_frame.hpp

enum class VideoLane : uint8_t {
    Canonical     = 0x01,   // 1200×2000 JPEG/RGBA — AI/マクロ/座標基準
    Presentation  = 0x02,   // 1920×1080 H.264 — 表示専用
};

enum class PixelStorage : uint8_t {
    RGBA8,         // CPU memory (AI解析 / fallback)
    D3D11_RGBA,    // DX11 texture RGBA (Canonical upload後)
    D3D11_NV12,    // DX11 texture NV12 (Presentation HW decode)
};

struct VideoFrame {
    uint64_t      frame_id   = 0;
    uint64_t      pts_us     = 0;
    uint32_t      width      = 0;
    uint32_t      height     = 0;
    VideoLane     lane       = VideoLane::Canonical;
    PixelStorage  storage    = PixelStorage::RGBA8;
    bool          keyframe   = false;

    // GPU path
    void* d3d11_texture = nullptr;   // ID3D11Texture2D*
    void* d3d11_srv     = nullptr;   // ID3D11ShaderResourceView*

    // CPU path (AI解析・fallback)
    std::shared_ptr<uint8_t[]> rgba_data;
};
```

**使用ルール:**
- AI/OCR/マクロ → `lane == Canonical` のみ受け付ける
- 表示ルーティング → `Presentation` があれば優先、なければ `Canonical`
- 操作座標 → 常に `1200×2000` 基準、`Presentation` フレーム表示中でも変換しない

---

## フレームドロップポリシー

```
[受信側 UDP]
  frame_id が現在より古い               → 即破棄
  100ms 以上古いフラグメントバッファ    → flush + IDR要求

[デコード側]
  Canonical decode queue 上限: 2フレーム
  Presentation decode queue 上限: 2フレーム
  溢れたら古い方を破棄

[表示側]
  常に最新フレームのみ参照
  前フレーム再利用なし
```

---

## Android ファイル構成

```
android/capture/src/main/java/com/mirage/capture/
  stream/
    StreamService.kt          # Foreground Service
    ProjectionSession.kt      # MediaProjection保持
    BootReceiver.kt           # 自動起動
  encode/
    EncoderController.kt      # 2レーン管理 (VD作成/破棄)
    CanonicalEncoder.kt       # ImageReader→JPEG (Canonical Lane)
    PresentationEncoder.kt    # SurfaceRepeater→H.264 (Presentation Lane)
  transport/
    UdpFrameSender.kt         # UDP送信・フラグメント化 (両Lane共通)
    TcpControlServer.kt       # 制御TCP
  protocol/
    StreamProtocol.kt         # ヘッダ定数・コマンド定数
```

**削除:**
- `MjpegStreamer.kt`, `MjpegEglCore.kt` → `CanonicalEncoder.kt`に統合
- `RtpH264Packetizer.kt` → 廃止 (RTP廃止)
- 旧`UdpVideoSender.kt` → `UdpFrameSender.kt`に置き換え

---

## PC ファイル構成

```
src/
  stream/
    x1_session.hpp / .cpp           # 接続管理・再接続
    tcp_control_client.hpp / .cpp   # 制御チャネル
    udp_video_receiver.hpp / .cpp   # 受信・フラグメント再構築 (Lane別)
  video/
    video_frame.hpp                 # VideoFrame / VideoLane / PixelStorage
    frame_ring.hpp                  # リングバッファ (2フレーム)
    canonical_decoder.hpp / .cpp    # JPEG→RGBA→DX11
    presentation_decoder.hpp / .cpp # MF H.264 HW→D3D11 NV12
    video_presenter.hpp / .cpp      # 表示ルーティング (Pres優先/Canonical fallback)
  adapter/
    x1_stream_adapter.hpp / .cpp    # MultiDeviceReceiver接続口
```

**維持する既存資産:**
- `adb_device_manager.*`, `event_bus.*`, GUI骨格, AI/OCR上位

---

## 実装フェーズ

### Phase 1: Canonical Lane疎通
1. `StreamProtocol.kt` — ヘッダ/コマンド定数
2. `CanonicalEncoder.kt` — ImageReader→JPEG (MjpegStreamer v5ベース)
3. `UdpFrameSender.kt` — フラグメント化UDP送信
4. `TcpControlServer.kt` — START/PING/BYE/IDR
5. PC: `udp_video_receiver` — 受信・再構築
6. PC: `canonical_decoder` — JPEG→RGBA→DX11
7. PC: `x1_session` — 接続管理
8. PC: `video_presenter` — 既存GUIへ表示
→ 目標: **1200×2000 @ 30fps 疎通確認**

### Phase 2: AI/マクロ接続
- `CanonicalFrame` をAI Engineへ流す
- 座標系確認 (1200×2000 基準で操作一致)
- OCR/テンプレート精度確認

### Phase 3: Presentation Lane追加
1. `PresentationEncoder.kt` — SurfaceRepeater→H.264
2. PC: `presentation_decoder` — MF HW→D3D11 NV12
3. `PRES_ON/OFF` コマンド実装
4. `video_presenter` に表示ルーティング追加
→ 目標: **1920×1080 @ 60fps 表示確認（AI入力は変化しないこと確認）**

### Phase 4: 安定化・統合
- 再接続
- BootReceiver (USBデバッグなし自動起動)
- `x1_stream_adapter` → MultiDeviceReceiver統合
- A9系への水平展開検討
