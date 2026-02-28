#!/usr/bin/env python3
"""
Protocol.kt 同期チェッカー
3箇所のProtocol.ktが同期しているか検証

チェック対象:
1. src/mirage_protocol.hpp (PC側)
2. android/accessory/.../Protocol.kt
3. android/capture/.../Protocol.kt

Usage:
    python scripts/check_protocol_sync.py
    Exit code 0 = 同期OK, 1 = 差異あり
"""
import re
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent

# ファイルパス
PC_PROTOCOL = PROJECT_ROOT / "src" / "mirage_protocol.hpp"
ACCESSORY_PROTOCOL = PROJECT_ROOT / "android" / "accessory" / "src" / "main" / "java" / "com" / "mirage" / "accessory" / "usb" / "Protocol.kt"
CAPTURE_PROTOCOL = PROJECT_ROOT / "android" / "capture" / "src" / "main" / "java" / "com" / "mirage" / "capture" / "usb" / "Protocol.kt"

def extract_commands_from_hpp(filepath: Path) -> dict:
    """mirage_protocol.hppからコマンド定義を抽出"""
    if not filepath.exists():
        return {}

    content = filepath.read_text(encoding="utf-8", errors="ignore")
    commands = {}

    # パターン: CMD_XXX = 0xNN または CMD_XXX = N
    pattern = r"CMD_(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)"
    for match in re.finditer(pattern, content):
        name = match.group(1)
        value = match.group(2)
        # 16進数を統一
        if value.startswith("0x"):
            value = int(value, 16)
        else:
            value = int(value)
        commands[name] = value

    return commands

def extract_commands_from_kt(filepath: Path) -> dict:
    """Protocol.ktからコマンド定義を抽出"""
    if not filepath.exists():
        return {}

    content = filepath.read_text(encoding="utf-8", errors="ignore")
    commands = {}

    # パターン: const val CMD_XXX: Byte = 0xNN または N
    pattern = r"const\s+val\s+CMD_(\w+):\s*Byte\s*=\s*(0x[0-9A-Fa-f]+|\d+)"
    for match in re.finditer(pattern, content):
        name = match.group(1)
        value = match.group(2)
        # .toByte()を除去、16進数を統一
        value = value.replace(".toByte()", "")
        if value.startswith("0x"):
            value = int(value, 16)
        else:
            value = int(value)
        commands[name] = value

    return commands

def compare_commands(pc_cmds: dict, accessory_cmds: dict, capture_cmds: dict) -> list:
    """3つのコマンドセットを比較し、差異を報告"""
    issues = []

    # 全コマンド名を収集
    all_cmds = set(pc_cmds.keys()) | set(accessory_cmds.keys()) | set(capture_cmds.keys())

    for cmd in sorted(all_cmds):
        pc_val = pc_cmds.get(cmd)
        acc_val = accessory_cmds.get(cmd)
        cap_val = capture_cmds.get(cmd)

        # 存在チェック
        missing = []
        if pc_val is None:
            missing.append("mirage_protocol.hpp")
        if acc_val is None:
            missing.append("accessory/Protocol.kt")
        if cap_val is None:
            missing.append("capture/Protocol.kt")

        if missing:
            issues.append(f"CMD_{cmd}: missing in {', '.join(missing)}")
            continue

        # 値の一致チェック
        if not (pc_val == acc_val == cap_val):
            issues.append(
                f"CMD_{cmd}: value mismatch - "
                f"hpp=0x{pc_val:02X}, accessory=0x{acc_val:02X}, capture=0x{cap_val:02X}"
            )

    return issues

def main():
    print("Checking Protocol.kt synchronization...")
    print(f"  PC:        {PC_PROTOCOL}")
    print(f"  Accessory: {ACCESSORY_PROTOCOL}")
    print(f"  Capture:   {CAPTURE_PROTOCOL}")
    print()

    # ファイル存在チェック
    missing_files = []
    if not PC_PROTOCOL.exists():
        missing_files.append(str(PC_PROTOCOL))
    if not ACCESSORY_PROTOCOL.exists():
        missing_files.append(str(ACCESSORY_PROTOCOL))
    if not CAPTURE_PROTOCOL.exists():
        missing_files.append(str(CAPTURE_PROTOCOL))

    if missing_files:
        print("ERROR: Missing files:")
        for f in missing_files:
            print(f"  - {f}")
        sys.exit(1)

    # コマンド抽出
    pc_cmds = extract_commands_from_hpp(PC_PROTOCOL)
    accessory_cmds = extract_commands_from_kt(ACCESSORY_PROTOCOL)
    capture_cmds = extract_commands_from_kt(CAPTURE_PROTOCOL)

    print(f"Commands found: PC={len(pc_cmds)}, Accessory={len(accessory_cmds)}, Capture={len(capture_cmds)}")

    # 比較
    issues = compare_commands(pc_cmds, accessory_cmds, capture_cmds)

    if issues:
        print("\nSynchronization issues found:")
        for issue in issues:
            print(f"  - {issue}")
        print("\nPlease ensure all three Protocol files are synchronized.")
        sys.exit(1)
    else:
        print("\nAll Protocol files are synchronized.")
        sys.exit(0)

if __name__ == "__main__":
    main()
