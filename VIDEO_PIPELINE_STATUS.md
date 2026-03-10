# Video Pipeline Status
> Updated: 2026-03-10 | 実機APKおよびソースコードを直接確認済み

---

## 現状サマリー

| レイヤー | コーデック | 状態 |
|---------|-----------|------|
| Android エンコーダ (実機APK) | **H.265/HEVC** | ✅ 動作中 |
| Android RTPパケット化 | **HEVC FU-A (RFC 7798)** | ✅ 実装済み |
| PC 受信・コーデック自動検出 | HEVC自動判定 | ✅ 実装済み |
| PC デコーダ (FFmpeg) | H.265/HEVC | ✅ 実装済み |
| PC デコーダ (Vulkan Video) | H.264のみ (H.265未対応) | ⚠️ HEVC→FFmpegフォールバック |

---

## Android側 (送信) — MirageVulkan / android/capture/

### X1インストール済みAPK情報
- パッケージ: `com.mirage.capture`
- versionCode: 1 / versionName: 1.0.0
- lastUpdateTime: **2026-03-10 01:26:28** (本日更新済み)
- firstInstallTime: 2026-03-07 14:04:20

### H264Encoder.kt (実機・未コミット変更適用済み)
```
TAG = "MirageH265"                            ← H.265に移行済み
mimeType() = MediaFormat.MIMETYPE_VIDEO_HEVC  ← H.265固定
I_FRAME_INTERVAL = 5s                         ← MTK HEVC参照フレームクリア対策
エンコーダ優先順位:
  1. OMX.google.hevc.encoder (SW HEVC)
  2. MediaCodec.createEncoderByType(HEVC)  ← HWフォールバック
```

### RtpH264Packetizer.kt (HEVC対応済み)
```
useHevc = true  ← H264Encoderから渡される
VPS(type=32) / SPS(type=33) / PPS(type=34) キャッシュ
IDR前にVPS/SPS/PPS自動付与
packetizeHevcFu(): 2バイトHEVC NALヘッダ処理 (RFC 7798)
```

### AnnexBSplitter.kt
- H.265 NALタイプ判定の実装状況は未確認 → 要確認

### 送信パス
```
VirtualDisplay → SurfaceRepeater(EGL) → MediaCodec(HEVC)
  → AnnexBSplitter → RtpH264Packetizer(useHevc=true)
  → UsbVideoSender(VID0) / TcpVideoSender / UdpVideoSender
```

---

## PC側 (受信・デコード) — MirageVulkan / src/

### mirror_receiver.cpp
```cpp
bool stream_is_hevc_ = false;  // RTPのNALタイプから自動検出
// VPS/SPS/PPS(32/33/34)検出 → stream_is_hevc_ = true
// HEVC FU reassembly (RFC 7798) 実装済み
// For HEVC: always use UnifiedDecoder (H264Decoder単体はHEVC不可)
```

### unified_decoder.cpp / unified_decoder.hpp
```cpp
enum class VideoCodec { H264, HEVC };
// NOTE: HEVC currently uses FFmpeg backend
// (Vulkan Video path is H.264-only)
config.codec = stream_is_hevc_ ? VideoCodec::HEVC : VideoCodec::H264;
```

### h264_decoder.cpp (FFmpeg)
```cpp
bool init(bool use_hevc = false);
avcodec_find_decoder(use_hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264)
// MediaTek HEVC VPS非準拠対策: AV_CODEC_FLAG2_IGNORE_CROP
// カラーレンジ修正: HEVC全てfull range強制 (washed-out色対策)
bool is_hevc_ = false;  // カラー変換分岐に使用
```

---

## 完全パイプライン図 (現状)

```
[Android X1]                              [PC / MirageVulkan GUI]
H264Encoder.kt                            mirror_receiver.cpp
  MIMETYPE_VIDEO_HEVC                       stream_is_hevc_ 自動検出
  ↓                                         ↓
RtpH264Packetizer.kt (useHevc=true)       unified_decoder.cpp
  VPS/SPS/PPS付与                           VideoCodec::HEVC
  HEVC FU-A (RFC 7798)                      ↓
  ↓                                        h264_decoder.cpp
TcpVideoSender / UdpVideoSender             AV_CODEC_ID_HEVC (FFmpeg)
  TCP: adb forward tcp:50100                MTK色補正
  UDP: port 60000-60009                     ↓
                                           RGBA → GUI表示
```

---

## 未確認・要対応事項

| 項目 | 内容 | 優先度 |
|------|------|--------|
| AnnexBSplitter.kt | HEVCのNALタイプ(VPS=32等)を正しく判定しているか | 🔴 要確認 |
| X1での実動作 | エンコード→GUI表示のE2Eテスト未実施 | 🔴 要確認 |
| Vulkan Video HEVC | 現状H.264のみ対応、HEVCはFFmpegフォールバック | 🟡 将来対応 |
| git未コミット変更 | H264Encoder.kt他8ファイルが未コミット | 🟡 要コミット |

---

## git未コミット変更ファイル一覧 (2026-03-10時点)

```
M android/capture/src/main/AndroidManifest.xml
M android/capture/src/main/java/com/mirage/capture/access/DebugCommandReceiver.kt
M android/capture/src/main/java/com/mirage/capture/boot/CaptureBootReceiver.kt
M android/capture/src/main/java/com/mirage/capture/capture/H264Encoder.kt  ← HEVC移行
M android/capture/src/main/java/com/mirage/capture/capture/ScreenCaptureService.kt
M android/capture/src/main/java/com/mirage/capture/capture/TcpVideoSender.kt
M android/capture/src/main/java/com/mirage/capture/ipc/AccessoryCommandReceiver.kt
M android/capture/src/main/java/com/mirage/capture/ui/AccessoryActivity.kt
M android/capture/src/main/java/com/mirage/capture/ui/CaptureActivity.kt
M CMakeLists.txt / config.json / scripts/* / src/*
```
