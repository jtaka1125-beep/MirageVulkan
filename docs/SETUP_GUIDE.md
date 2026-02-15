# MirageTestKit v2.0 セットアップガイド

## 目次
1. [必要要件](#必要要件)
2. [クイックスタート](#クイックスタート)
3. [PC環境構築](#pc環境構築)
4. [Androidアプリビルド](#androidアプリビルド)
5. [設定ファイル](#設定ファイル)
6. [トラブルシューティング](#トラブルシューティング)

---

## 必要要件

### PC側
- Windows 10/11 (64bit)
- MSYS2 (https://www.msys2.org/)
- Python 3.10+
- Android SDK (adb)

### Android側
- Android 10以上
- USBデバッグ有効
- 開発者オプション有効

---

## クイックスタート

### 1. 依存関係インストール（自動）

```bash
# MSYS2 MinGW64ターミナルで実行
./setup_build_env.sh
```

### 2. GUI ビルド

```bash
cd gui_imgui
mkdir -p build && cd build
cmake -G Ninja ..
ninja
```

### 3. Androidアプリビルド

```bash
cd android/MirageAndroid
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### 4. 設定ファイル編集

`config.json` を編集してPCのIPアドレスを設定:
```json
{
    "network": {
        "pc_ip": "192.168.0.8"  // ← 自分のPCのIPに変更
    }
}
```

### 5. 起動

```bash
# PC側: GUIビューア起動
./build/mirage_gui.exe

# または Python版
cd scripts
python hybrid_video_viewer.py
```

---

## PC環境構築

### MSYS2 インストール

1. https://www.msys2.org/ からインストーラをダウンロード
2. インストール後、「MSYS2 MinGW64」を起動
3. パッケージ更新:
   ```bash
   pacman -Syu
   ```

### 依存パッケージ（手動インストール）

```bash
# ビルドツール
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

# FFmpeg (動画デコード)
pacman -S mingw-w64-x86_64-ffmpeg

# OpenCV (画像処理/AI)
pacman -S mingw-w64-x86_64-opencv

# Tesseract OCR
pacman -S mingw-w64-x86_64-tesseract-ocr
pacman -S mingw-w64-x86_64-tesseract-data-eng
pacman -S mingw-w64-x86_64-tesseract-data-jpn

# libusb (USB AOA)
pacman -S mingw-w64-x86_64-libusb

# OpenCL (GPU高速化) - オプション
pacman -S mingw-w64-x86_64-opencl-icd mingw-w64-x86_64-opencl-headers
```

### Python パッケージ

```bash
pip install pytesseract opencv-python pillow pyusb
```

### 機能別ビルドオプション

CMakeオプションで機能を有効/無効化:

```bash
# 全機能有効（デフォルト）
cmake -G Ninja ..

# FFmpegなし（動画表示不可）
cmake -G Ninja -DUSE_FFMPEG=OFF ..

# AI機能なし
cmake -G Ninja -DUSE_AI=OFF ..

# OCR機能なし
cmake -G Ninja -DUSE_OCR=OFF ..

# USB AOA無効
cmake -G Ninja -DUSE_LIBUSB=OFF ..

# 最小構成（WiFiビューアのみ）
cmake -G Ninja -DUSE_AI=OFF -DUSE_OCR=OFF -DUSE_LIBUSB=OFF ..
```

---

## Androidアプリビルド

### 必要なもの
- Android Studio または Gradle
- Android SDK 30以上
- JDK 11以上

### ビルド手順

```bash
cd android/MirageAndroid

# デバッグビルド
./gradlew assembleDebug

# APKの場所
# app/build/outputs/apk/debug/app-debug.apk
```

### インストール

```bash
# 単一デバイス
adb install -r app/build/outputs/apk/debug/app-debug.apk

# 複数デバイス
adb devices  # シリアル番号確認
adb -s <SERIAL> install -r app/build/outputs/apk/debug/app-debug.apk
```

### 権限設定

アプリインストール後、以下の権限を設定:

1. **アクセシビリティサービス**（必須）
   - 設定 → ユーザー補助 → MirageAccessibilityService → ON

2. **バッテリー最適化除外**（推奨）
   - 設定 → アプリ → Mirage → バッテリー → 最適化しない

ADBで自動設定も可能:
```bash
python scripts/auto_setup_devices.py --setup-permissions
```

---

## 設定ファイル

### config.json（メイン設定）

```json
{
    "network": {
        "pc_ip": "192.168.0.8",       // PCのIPアドレス
        "video_base_port": 60000,      // 映像用ベースポート
        "command_base_port": 50000,    // コマンド用ベースポート
        "tcp_command_port": 50100      // TCP制御ポート
    },
    "usb_tether": {
        "android_ip": "192.168.42.129" // USBテザリング時のAndroid IP
    },
    "gui": {
        "window_width": 1920,
        "window_height": 1080,
        "vsync": true
    },
    "ai": {
        "enabled": true,
        "templates_dir": "templates",
        "default_threshold": 0.80
    },
    "ocr": {
        "enabled": false,              // CPU負荷が高いのでデフォルトOFF
        "language": "eng+jpn"
    }
}
```

### ポート割り当て

複数デバイス接続時のポート割り当て:

| デバイス | 映像ポート | コマンドポート |
|---------|-----------|---------------|
| 1台目   | 60000     | 50000         |
| 2台目   | 60001     | 50001         |
| 3台目   | 60002     | 50002         |
| ...     | ...       | ...           |

### IPアドレスの確認方法

```bash
# Windows
ipconfig | findstr IPv4

# Linux/Mac
ip addr | grep inet
```

---

## トラブルシューティング

### よくある問題

#### 1. 映像が表示されない

**原因**: ファイアウォールでUDPポートがブロックされている

**解決策**:
```bash
# Windows Firewall設定
netsh advfirewall firewall add rule name="MirageTestKit" dir=in action=allow protocol=UDP localport=60000-60010
```

または `scripts/add_firewall_rule.bat` を管理者権限で実行

#### 2. ADBでデバイスが見つからない

**確認**:
```bash
adb devices
```

**解決策**:
1. USBデバッグを有効にする
2. USBケーブルを変える（データ転送対応のもの）
3. ADBドライバをインストール

#### 3. アクセシビリティが有効にならない

一部のデバイスではADB経由での設定が制限されています。

**解決策**: 手動で設定
- 設定 → ユーザー補助 → インストールされたアプリ → MirageAccessibilityService → ON

#### 4. CMakeでFFmpegが見つからない

**エラー例**:
```
Could not find package configuration file provided by "ffmpeg"
```

**解決策**:
```bash
# pkg-config パスを確認
echo $PKG_CONFIG_PATH

# 設定されていない場合
export PKG_CONFIG_PATH=/mingw64/lib/pkgconfig:$PKG_CONFIG_PATH
```

#### 5. ビルド時のリンクエラー

**エラー例**:
```
undefined reference to `avcodec_open2'
```

**解決策**: FFmpegが正しくインストールされているか確認
```bash
pkg-config --libs libavcodec
```

#### 6. USB AOAが動作しない

**確認**:
```bash
# AOAデバイス一覧
./pc/list_aoa_devices.exe
```

**解決策**:
1. libusb-win32ドライバをインストール（Zadig使用）
2. `setup/drivers/` のドライバをインストール

---

## 接続方式

### USB AOA（推奨）
- 最も低遅延
- PC側で `aoa_switch.exe` 実行後、Androidアプリ起動

### WiFi（フォールバック）
- USB接続なしで動作
- 同一ネットワーク上で使用

### USBテザリング
- Android側でUSBテザリング有効化
- 自動的にAndroid IP (192.168.42.129) を検出

---

## ログ確認

### PC側
```bash
# GUIのコンソール出力
./build/mirage_gui_debug.exe

# Python版
python hybrid_video_viewer.py 2>&1 | tee log.txt
```

### Android側
```bash
adb logcat -s MirageMain:V MirageCapture:V MirageAccessoryIO:V
```
