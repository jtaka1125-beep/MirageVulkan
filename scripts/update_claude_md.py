#!/usr/bin/env python3
"""
PROJECT_STATE.md自動更新スクリプト
- GUI File Line Counts セクションを実際の行数で更新
- pre-commitフックから呼び出される

Usage:
    python scripts/update_claude_md.py [--check]
    --check: 更新が必要かどうかだけチェック（exitcode 1 = 要更新）
"""
import re
import sys
from datetime import datetime
from pathlib import Path

# プロジェクトルート
PROJECT_ROOT = Path(__file__).parent.parent
PROJECT_STATE = PROJECT_ROOT / "PROJECT_STATE.md"

# 行数を追跡するファイル (相対パス)
GUI_FILES = [
    "src/gui/gui_ai_panel.cpp",
    "src/gui/gui_init.cpp",
    "src/gui/gui_threads.cpp",
    "src/gui/gui_device_control.cpp",
    "src/gui/gui_command.cpp",
    "src/gui/gui_window.cpp",
    "src/gui/gui_main.cpp",
    "src/gui/gui_state.cpp",
    "src/mirage_context.cpp",
]

def count_lines(filepath: Path) -> int:
    """ファイルの行数をカウント"""
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            return sum(1 for _ in f)
    except FileNotFoundError:
        return 0

def get_current_line_counts() -> dict:
    """現在の各ファイルの行数を取得"""
    counts = {}
    for rel_path in GUI_FILES:
        full_path = PROJECT_ROOT / rel_path
        name = Path(rel_path).name
        counts[name] = count_lines(full_path)
    return counts

def extract_current_total(content: str) -> int:
    """PROJECT_STATE.mdから現在のTOTAL行数を抽出"""
    match = re.search(r"- TOTAL:\s+(\d+)\s+lines", content)
    if match:
        return int(match.group(1))
    return 0

def update_project_state(counts: dict, check_only: bool = False) -> bool:
    """
    PROJECT_STATE.mdの行数セクションを更新
    Returns: True if updated (or needs update in check mode)
    """
    if not PROJECT_STATE.exists():
        print(f"ERROR: {PROJECT_STATE} not found")
        return False

    content = PROJECT_STATE.read_text(encoding="utf-8")
    today = datetime.now().strftime("%Y-%m-%d")

    # 新しい行数を計算
    total = sum(counts.values())

    # 現在のTOTALと比較
    current_total = extract_current_total(content)
    if current_total == total:
        print(f"PROJECT_STATE.md line counts are up to date (TOTAL: {total})")
        return False

    if check_only:
        print("PROJECT_STATE.md line counts need update:")
        print(f"  Current total: {current_total}")
        print(f"  Actual total:  {total}")
        return True

    # セクション全体を置換
    # パターン: ## GUI File Line Counts から次の ## まで
    section_start = content.find("## GUI File Line Counts")
    if section_start == -1:
        print("WARNING: GUI File Line Counts section not found")
        return False

    section_end = content.find("\n## ", section_start + 1)
    if section_end == -1:
        section_end = len(content)

    # 新しいセクションを生成
    new_section = f"## GUI File Line Counts (Updated {today})\n"
    for rel_path in GUI_FILES:
        name = Path(rel_path).name
        count = counts[name]
        padding = " " * (22 - len(name))  # 固定幅
        new_section += f"- {name}:{padding}{count:>4}\n"
    new_section += f"- TOTAL:                 {total:>4} lines\n"

    # 更新
    new_content = content[:section_start] + new_section + content[section_end:]
    PROJECT_STATE.write_text(new_content, encoding="utf-8")
    print(f"PROJECT_STATE.md updated: TOTAL {total} lines (was {current_total})")
    return True

def main():
    check_only = "--check" in sys.argv

    counts = get_current_line_counts()

    if check_only:
        print("Checking PROJECT_STATE.md line counts...")
    else:
        print("Updating PROJECT_STATE.md line counts...")

    needs_update = update_project_state(counts, check_only)

    if check_only and needs_update:
        print("\nRun 'python scripts/update_claude_md.py' to update")
        sys.exit(1)

    sys.exit(0)

if __name__ == "__main__":
    main()
