# Signature Operational Guide
## MirageSystem v2 WinUSB AOA - 署名・証明書運用ルール

**重要度**: ★★★★★ - 本番化前に必読  
**対象**: システム管理者・デプロイメント担当  
**作成日**: 2026-02-02

---

## 📋 **概要**

Windows は 64-bit OS における **ドライバのコード署名を必須**としています。

MirageSystem v2 の WinUSB ドライバ統合では、以下のいずれかの署名戦略を取る必要があります：

```
戦略1: テストモード環境
   ├─ Windows を "署名要件なし" で動作
   └─ 開発・テスト用途向け

戦略2: 証明書登録（自己署名）
   ├─ WDI (wdi-simple.exe) で証明書生成
   ├─ TrustedPublisher ストアに登録
   └─ 社内・個人限定デプロイ向け

戦略3: 正規署名（WHQL / Attestation）
   ├─ Microsoft WHQL 認証取得
   ├─ または Attestation署名（EV 証明書）
   └─ 本番・大規模配布向け

本ドキュメントは **戦略1 と 戦略2** の実装ガイドです。
```

---

## 🎯 **戦略選択フロー**

```
【署名戦略の選択】

開発・テスト環境？
    ├─ YES → 戦略1（テストモード） ← 最速
    └─ NO → 社内・限定配布？
        ├─ YES → 戦略2（自己署名証明書） ← 推奨
        └─ NO → 戦略3（WHQL署名） ← 本番大規模

【本ガイドが対象】
━━━━━━━━━━━━━━━━━
戦略1: テストモード環境
戦略2: 自己署名証明書
━━━━━━━━━━━━━━━━━
```

---

## 戦略1️⃣ : テストモード環境

### **利点**

- ✅ セットアップ簡単（1コマンド）
- ✅ 署名なしで動作
- ✅ 開発・テスト向け

### **欠点**

- ❌ 本番環境では使えない
- ❌ 再起動時に"テストモード"警告が表示
- ❌ セキュリティポリシーによっては禁止

### **実装（管理者 PowerShell）**

```powershell
# テストモード有効化
bcdedit /set testsigning on

# 再起動
Restart-Computer -Force

# テストモード確認（左下に "Test Mode" と表示）
# テストモード無効化する場合
bcdedit /set testsigning off
```

### **使用環境**

```
✓ 個人開発マシン
✓ CI/CD テスト環境
✓ 研究室環境
✓ 社内開発チーム（テスト段階）
```

### **注意**

- 再起動が必須
- 無効化もテストモード状態で実行
- 本番環境には絶対に持ち込まない

---

## 戦略2️⃣ : 自己署名証明書（推奨）

### **利点**

- ✅ テストモード不要
- ✅ 警告表示なし
- ✅ 社内・限定配布向け
- ✅ 証明書登録は一度だけ

### **欠点**

- ❌ セットアップに手数がかかる
- ❌ 証明書の有効期限管理が必要
- ❌ 証明書の配布が必要

### **前提条件**

```
✓ libwdi ビルド済み (wdi-simple.exe + 証明書)
✓ Windows SDK (inf2cat.exe で CAT 生成)
✓ PowerShell 実行権限
```

### **Step 1: WDI で証明書を生成**

```batch
REM libwdi から出力される証明書（自動生成）
REM wdi-simple.exe ビルド時に以下ファイルが生成される:

  libwdi\Win32\Release\examples\
    ├─ wdi-simple.exe
    ├─ libwdi.dll
    └─ wdi-driver.cat

REM これらを drivers/ ディレクトリにコピー
copy libwdi\Win32\Release\examples\wdi-driver.cat drivers\
copy libwdi\Win32\Release\examples\wdi-simple.exe drivers\
```

### **Step 2: CAT ファイルを生成（INF用）**

```batch
REM Windows SDK に付属の inf2cat.exe を使用
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x86\inf2cat.exe" ^
  /driver=. ^
  /os=10_x64

REM 実行後
android_accessory_interface.cat
  が生成される
```

### **Step 3: 証明書をシステムストアに登録**

```powershell
# 管理者 PowerShell で実行

# WDI の証明書を TrustedPublisher に登録
$oemCert = Get-Item drivers\wdi-driver.cat
$oemPfx = "drivers\wdi-driver.pfx"  # PFXファイルの場所

# または、INFの署名用証明書をインストール
Import-Certificate -FilePath $oemPfx -CertStoreLocation Cert:\LocalMachine\TrustedPublisher

# 確認
Get-ChildItem -Path Cert:\LocalMachine\TrustedPublisher | Select-Object Subject, Thumbprint
```

### **Step 4: ドライバをインストール**

```batch
REM pnputil で署名済み CAT と一緒にインストール
pnputil /add-driver android_accessory_interface.inf /install
```

---

## 戦略3️⃣ : WHQL署名（本番向け - 参考）

> ⚠️ 本ガイドでは詳細実装なし。以下は概略のみ。

### **プロセス**

```
1. Microsoft へのドライバ申請
   ├─ WHQL テスト申請フォーム提出
   └─ テスト実施（4-8週間）

2. テスト合格後
   ├─ Microsoft 署名入手
   └─ CAT ファイル取得

3. ドライバ配布
   ├─ Windows Update 対応
   └─ またはメーカーサイト配布
```

### **必要なもの**

- ✓ Windows Hardware Lab Kit (HLK)
- ✓ テスト環境
- ✓ Microsoft Partner アカウント

### **コスト**

- Microsoft WHQL: 有料（コース別）

### **用途**

- ✓ OEM 配布
- ✓ 商用製品
- ✓ Windows Update 対応

---

## 🚀 **実装ガイド（戦略2推奨）**

### **社内デプロイメント用セットアップ**

```powershell
# PowerShell (管理者権限)

# Step 1: 証明書ストアの確認
Get-ChildItem -Path Cert:\LocalMachine\TrustedPublisher

# Step 2: WDI 証明書をインストール
$cert = Get-Item "drivers\wdi-driver.pfx"
Import-Certificate -FilePath $cert.FullName `
  -CertStoreLocation Cert:\LocalMachine\TrustedPublisher `
  -Password (ConvertTo-SecureString -String "password" -AsPlainText -Force)

# Step 3: インストール確認
Get-ChildItem -Path Cert:\LocalMachine\TrustedPublisher | 
  Where-Object { $_.Subject -like "*wdi*" }

# Step 4: ドライバインストール
pnputil /add-driver drivers\android_accessory_interface.inf /install

# Step 5: 検証
.\verify_aoa_winusb.ps1
```

### **チーム配布用スクリプト**

```batch
@echo off
REM setup_with_signature.bat
REM 証明書登録 + ドライバインストール（自動化版）

echo [INFO] Installing certificate...
PowerShell -Command ^
  "Import-Certificate -FilePath drivers\wdi-driver.pfx " ^
  "-CertStoreLocation Cert:\LocalMachine\TrustedPublisher" ^
  "-Password (ConvertTo-SecureString -String 'password' -AsPlainText -Force)"

echo [INFO] Installing driver...
pnputil /add-driver drivers\android_accessory_interface.inf /install

echo [OK] Setup completed
```

---

## ⚠️ **署名関連トラブルシューティング**

### **Q: "The system cannot find the certificate"**

```
原因: 証明書パスが違う or パスワード間違い

解決:
1. 証明書ファイルの位置確認
   dir drivers\*.pfx
   
2. パスワード確認（ビルド時のログ参照）

3. 再度インポート
   Import-Certificate -FilePath (正しいパス)
```

### **Q: "Signature verification failed" (ドライバインストール失敗)**

```
原因: CAT ファイルが署名されていない or INF が .cat と対応していない

解決:
1. inf2cat.exe で再生成
   "C:\Program Files (x86)\Windows Kits\10\bin\...\inf2cat.exe" 
     /driver=. /os=10_x64

2. 証明書再インポート

3. ドライバ再インストール
```

### **Q: テストモードを無効化したい**

```powershell
# 管理者 PowerShell

bcdedit /set testsigning off
Restart-Computer -Force

# 再起動後 "Test Mode" 警告が消える
```

### **Q: 証明書を削除したい**

```powershell
# 管理者 PowerShell

# TrustedPublisher から削除
Get-ChildItem -Path Cert:\LocalMachine\TrustedPublisher | 
  Where-Object { $_.Subject -like "*wdi*" } | 
  Remove-Item

# 確認
Get-ChildItem -Path Cert:\LocalMachine\TrustedPublisher | 
  Select-Object Subject
```

---

## 📋 **本番化チェックリスト**

### **戦略1（テストモード）を採用する場合**

```
□ テストモード環境であることを明記
□ 本番環境への持ち込みを禁止
□ 運用ルール書に記載
□ チーム全員に周知
```

### **戦略2（自己署名証明書）を採用する場合**

```
□ 証明書ファイルの保管ルール確立
□ パスワード管理（KMS or 鍵管理ツール）
□ 有効期限管理計画
□ チーム内配布方法を決定
□ インストール手順をドキュメント化
□ トラブル時対応フロー確立
```

### **両戦略に共通**

```
□ ドライバインストール検証プロセス
□ ロールバック手順の確認
□ セキュリティ監査実施
□ 本番環境のセキュリティポリシー確認
□ 監視・ログ記録体制構築
```

---

## 🎯 **推奨構成（組織規模別）

### **個人・小規模チーム（1-5人）**

```
推奨: 戦略1（テストモード）
理由: セットアップ簡単、管理負担最小

構成:
├─ テスト機を特定
├─ テストモード有効化（1回）
└─ ドライバインストール

注意: 本番環境は絶対に使用禁止
```

### **部門・チーム（6-50人）**

```
推奨: 戦略2（自己署名証明書）
理由: バランスが取れている

構成:
├─ WDI ビルド（1回）
├─ 証明書をKMS で管理
├─ チーム全員に証明書配布
└─ 自動セットアップスクリプト提供

運用:
├─ 年1回証明書更新
├─ ログ監視
└─ 定期的な検証
```

### **エンタープライズ（51人以上）**

```
推奨: 戦略3（WHQL署名）+ GPO配布
理由: 長期運用・大規模配布向け

構成:
├─ WHQL 署名取得
├─ Windows Update 対応
├─ GPO でドライバ配布
└─ MDM で監視

運用:
├─ Microsoft のサポート利用可
├─ セキュリティパッチ対応
└─ 監査対応容易
```

---

## 📞 **参考資料**

| リソース | 説明 |
|---------|------|
| [Windows Hardware Certification Kit](https://docs.microsoft.com/en-us/windows-hardware/test/hlk/) | WHQL テストキット |
| [inf2cat.exe ドキュメント](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/inf2cat) | CAT ファイル生成 |
| [Test Signing](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/test-signing) | テストモード詳細 |
| [Driver Signing Policy](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/driver-signing) | 署名ポリシー全般 |

---

## ✅ **まとめ**

```
【署名の実装は3択】

1️⃣ テストモード（最速）
   用途: 開発・テスト
   手数: 最小
   
2️⃣ 自己署名証明書（推奨）
   用途: 社内・限定デプロイ
   手数: 中程度
   
3️⃣ WHQL署名（本番）
   用途: 大規模・商用
   手数: 最大

【本ドキュメント作成後の次ステップ】
┌─ 組織の実装戦略を選定
├─ 運用ルールを文書化
├─ チーム教育
└─ 本番化
```

---

**このドキュメントがないと、ドライバインストール失敗が環境差で再現不可になります。**

**本番化前に必ず確認してください。** 🔒
