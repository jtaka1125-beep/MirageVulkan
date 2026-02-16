#!/usr/bin/env python3
"""
export_analysis.py - 解析用zipを生成（機密データ除外）

出先でClaudeに読ませるための安全なエクスポート。
keystore, ADB鍵, ビルド成果物, 機密データを除外してzipに固める。

Usage:
    python export_analysis.py                    # 全体をエクスポート
    python export_analysis.py --src-only         # C++ソースのみ
    python export_analysis.py --android-only     # Androidソースのみ
    python export_analysis.py --scripts-only     # スクリプトのみ
    python export_analysis.py --output path.zip  # 出力先指定

出力先デフォルト: C:\\MirageWork\\analysis\\miragevulkan_YYYYMMDD_HHMMSS.zip
"""

import os
import sys
import re
import zipfile
import datetime
import argparse
import fnmatch

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ANALYSIS_DIR = os.path.join(os.path.dirname(ROOT), "analysis")

# 除外パターン（セキュリティ + ビルド成果物）
EXCLUDE_PATTERNS = [
    # 機密
    "*.keystore", "*.jks", "*.pem", "*.key",
    "adbkey", "adbkey.pub", "local.properties",
    # ビルド成果物
    "build/", ".gradle/",
    "*.apk", "*.idsig", "*.ab",
    "*.exe", "*.pdb", "*.ilk", "*.obj", "*.o", "*.a", "*.lib", "*.dll", "*.so", "*.spv",
    # IDE / 一時ファイル
    ".vs/", ".cache/", ".idea/",
    "*.user", "*.suo", "__pycache__/", "*.pyc", "node_modules/",
    # Git
    ".git/",
    # 大容量
    "logs/", "screenshots/", "third_party/", "analysis/",
]

INCLUDE_SETS = {
    "all": None,
    "src": ["src/", "include/", "shaders/", "tests/", "CMakeLists.txt", "config.json"],
    "android": ["android/"],
    "scripts": ["scripts/", "docs/"],
}


def should_exclude(rel_path):
    for pattern in EXCLUDE_PATTERNS:
        if pattern.endswith("/"):
            if pattern.rstrip("/") in rel_path.replace("\\", "/").split("/"):
                return True
        elif fnmatch.fnmatch(os.path.basename(rel_path), pattern):
            return True
    return False


def should_include(rel_path, include_filter):
    if include_filter is None:
        return True
    rel_unix = rel_path.replace("\\", "/")
    for prefix in include_filter:
        if prefix.endswith("/"):
            if rel_unix.startswith(prefix) or ("/" + prefix) in ("/" + rel_unix):
                return True
        else:
            if rel_unix == prefix or rel_unix.endswith("/" + prefix):
                return True
    return False


def scan_files(root, include_filter):
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for fname in filenames:
            full_path = os.path.join(dirpath, fname)
            rel_path = os.path.relpath(full_path, root)
            if should_exclude(rel_path):
                continue
            if not should_include(rel_path, include_filter):
                continue
            files.append((full_path, rel_path))
    return files


def create_zip(files, output_path):
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for full_path, rel_path in files:
            zf.write(full_path, f"MirageVulkan/{rel_path}")
    return os.path.getsize(output_path)


def security_scan(files):
    """機密データが含まれていないか最終チェック
    
    検出対象:
      - 実際のパスワード値 (mirage123等)
      - パスワード/シークレットの直接代入 (環境変数参照は除外)
    誤検知除外:
      - 変数名に含まれる password/token (storePassword等)
      - 環境変数参照 (System.getenv, os.environ)
      - export_analysis.py 自身
      - ライセンスファイル、ドキュメント
    """
    issues = []

    # 実際の機密値
    exact_secrets = ["mirage123"]

    # 直接代入パターン: "password" = "実値" (環境変数でないもの)
    direct_assign_re = re.compile(
        r'(?:store[Pp]assword|key[Pp]assword|api[_]?key|secret)\s*[=:]\s*"([^"]{3,})"'
    )

    # 安全なパターン（環境変数参照）
    safe_refs = ["System.getenv", "getenv(", "os.environ", "process.env"]

    for full_path, rel_path in files:
        # 自分自身・ライセンス・ドキュメントはスキップ
        basename = os.path.basename(rel_path)
        if basename == "export_analysis.py":
            continue
        if basename.endswith((".md", ".txt")) and any(
            d in rel_path for d in ["license", "docs/", "archive/"]
        ):
            continue

        try:
            with open(full_path, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
        except:
            continue

        # 実値チェック
        for secret in exact_secrets:
            if secret in content:
                issues.append(f"  ! {rel_path}: 平文パスワード '{secret}' を検出")

        # 直接代入チェック
        for match in direct_assign_re.finditer(content):
            value = match.group(1)
            line = match.group(0)
            # 環境変数参照なら安全
            if any(ref in line for ref in safe_refs):
                continue
            # 空文字やプレースホルダは無視
            if value in ("", "YOUR_PASSWORD_HERE", "changeme"):
                continue
            issues.append(f"  ! {rel_path}: 機密値の直接代入: {line[:80]}")

    return issues


def main():
    parser = argparse.ArgumentParser(description="解析用zipエクスポート")
    parser.add_argument("--src-only", action="store_true")
    parser.add_argument("--android-only", action="store_true")
    parser.add_argument("--scripts-only", action="store_true")
    parser.add_argument("--output", help="出力先zipパス")
    parser.add_argument("--no-scan", action="store_true", help="セキュリティスキャンスキップ")
    args = parser.parse_args()

    if args.src_only:
        scope = "src"
    elif args.android_only:
        scope = "android"
    elif args.scripts_only:
        scope = "scripts"
    else:
        scope = "all"

    include_filter = INCLUDE_SETS[scope]

    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    output_path = args.output or os.path.join(ANALYSIS_DIR, f"miragevulkan_{scope}_{timestamp}.zip")

    print(f"=== MirageVulkan Analysis Export ===")
    print(f"スコープ: {scope}")
    print(f"出力先: {output_path}\n")

    files = scan_files(ROOT, include_filter)
    print(f"対象ファイル: {len(files)} 件")

    if not args.no_scan:
        print("\n[Security Scan]")
        issues = security_scan(files)
        if issues:
            print(f"  警告 {len(issues)} 件:")
            for issue in issues:
                print(issue)
            print("\n  続行する場合は --no-scan を付けてください")
            sys.exit(1)
        else:
            print("  [OK] 機密データ検出なし")

    print("\n[Creating ZIP]")
    size = create_zip(files, output_path)
    size_mb = size / 1024 / 1024

    print(f"\n完了: {output_path}")
    print(f"サイズ: {size_mb:.1f}MB ({len(files)} files)")
    print(f"\nこのzipをチャットにアップロードすれば解析可能です。")


if __name__ == "__main__":
    main()
