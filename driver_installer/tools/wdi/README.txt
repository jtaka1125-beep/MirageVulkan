# wdi-simple.exe 配置ガイド

## このフォルダについて

MirageUnifiedでWDI（Windows Driver Installer）方式を使用する場合、
このフォルダに `wdi-simple.exe` を配置してください。

## wdi-simple.exe の入手方法

### 方法1: libwdi プロジェクトからビルド（推奨）

1. libwdi リポジトリをクローン:
   ```
   git clone https://github.com/pbatard/libwdi.git
   ```

2. Visual Studio でビルド:
   ```
   cd libwdi
   libwdi.sln を開く → wdi-simple プロジェクトをビルド
   ```

3. 生成された `wdi-simple.exe` をこのフォルダにコピー

### 方法2: 事前ビルド済みバイナリ

Zadig (https://zadig.akeo.ie/) に同梱されているツールを使用:

1. Zadig をダウンロード
2. Zadig.exe と同じフォルダ内の関連ファイルを確認
3. 必要に応じてlibwdiのビルドを参照

## 配置後の確認

```
MirageUnified/
└── tools/
    └── wdi/
        ├── README.txt      (このファイル)
        └── wdi-simple.exe  ← ここに配置
```

## wdi-simple.exe がない場合

WDI方式は使用できませんが、代わりに pnputil 方式が自動的に
使用されます。pnputil 方式を使用する場合:

1. `android_accessory_interface.inf` ファイルが必要
2. テストモードまたは署名済みドライバが必要

## トラブルシューティング

### "wdi-simple.exe not found"
→ このフォルダに wdi-simple.exe を配置してください

### "Access denied"
→ 管理者として実行してください

### "Driver installation failed"
→ SIGNATURE_OPERATIONAL_GUIDE.md を参照してください

## 参考リンク

- libwdi: https://github.com/pbatard/libwdi
- Zadig: https://zadig.akeo.ie/
- WinUSB: https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb

