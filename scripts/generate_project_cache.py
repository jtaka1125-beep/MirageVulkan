#!/usr/bin/env python3
"""
generate_project_cache.py - プロジェクト構造キャッシュ生成

C++/Python/Kotlinソースを解析し、AIが高速に全体把握できる
構造化JSONを生成する。「外部構造記憶」として機能。

出力先:
  workspace/index/          - 構造キャッシュ
  workspace/review_cache/   - ファイル別評価用（空で初期化）

生成ファイル:
  project_manifest.json - コミット・スコープ・差分検知用
  class_index.json      - 全クラス・構造体一覧と層分類
  include_graph.json    - ヘッダ依存関係（正引き・逆引き）
  thread_map.json       - スレッド生成・mutex箇所
  layer_map.json        - 層別ファイル分類・統計
  file_summary.json     - ファイル別行数・複雑度ヒント

Usage:
    python generate_project_cache.py
"""

import os
import re
import json
import glob
import subprocess
import datetime

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC_DIR = os.path.join(ROOT, "src")
WORKSPACE = r"C:\MirageWork\mcp-server\workspace"
INDEX_DIR = os.path.join(WORKSPACE, "index")
REVIEW_DIR = os.path.join(WORKSPACE, "review_cache")

# 層定義
LAYER_KEYWORDS = {
    "video": ["video", "h264", "decoder", "encoder", "frame", "mirror", "vulkan_video",
              "yuv", "rgba", "vid0"],
    "transport": ["receiver", "sender", "usb_video", "tcp_video", "udp", "hybrid_receiver",
                  "multi_device_receiver"],
    "control": ["command_sender", "aoa_hid", "touch", "adb_touch", "hybrid_command",
                "multi_usb_command", "wifi_command", "usb_command"],
    "device": ["device_registry", "adb_device", "auto_setup", "route_controller"],
    "gui": ["gui_application", "gui_render", "gui_main", "mirage_context"],
    "gpu": ["vulkan_compute", "vulkan_template", "vulkan_context", "vulkan_swapchain",
            "vulkan_texture", "vulkan_image", "template_match", "prefix_sum", "pyramid"],
    "protocol": ["mirage_protocol", "mira_protocol", "event_bus", "ipc_client"],
    "config": ["config_loader", "mirage_config"],
    "infra": ["bandwidth_monitor", "rtt_tracker", "frame_dispatcher", "mirage_log", "result"],
}


def classify_layer(filename):
    base = os.path.splitext(os.path.basename(filename))[0].lower()
    for layer, keywords in LAYER_KEYWORDS.items():
        for kw in keywords:
            if kw in base:
                return layer
    return "other"


def extract_classes(filepath):
    classes = []
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            for i, line in enumerate(f, 1):
                m = re.match(r'^(class|struct)\s+(\w+)', line)
                if m:
                    if ";" in line and "{" not in line:
                        continue
                    classes.append({
                        "name": m.group(2), "kind": m.group(1), "line": i,
                        "file": os.path.relpath(filepath, ROOT).replace("\\", "/"),
                    })
    except:
        pass
    return classes


def extract_includes(filepath):
    includes = []
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                m = re.match(r'#include\s+"([^"]+)"', line)
                if m:
                    includes.append(m.group(1))
    except:
        pass
    return includes


def extract_threads(filepath):
    threads = []
    patterns = [
        (r'std::thread\s*\(', "std::thread()"),
        (r'std::async\s*\(', "std::async()"),
        (r'std::thread\s+\w+', "std::thread member"),
    ]
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            for i, line in enumerate(f, 1):
                for pat, kind in patterns:
                    if re.search(pat, line):
                        threads.append({
                            "line": i, "kind": kind,
                            "code": line.strip()[:120],
                            "file": os.path.relpath(filepath, ROOT).replace("\\", "/"),
                        })
    except:
        pass
    return threads


def extract_mutexes(filepath):
    mutexes = []
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            for i, line in enumerate(f, 1):
                if re.search(r'std::mutex|std::lock_guard|std::unique_lock|std::shared_mutex', line):
                    mutexes.append({"line": i, "code": line.strip()[:120]})
    except:
        pass
    return mutexes


def count_lines(filepath):
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.readlines()
            total = len(lines)
            code = len([l for l in lines if l.strip() and not l.strip().startswith("//")])
            return total, code
    except:
        return 0, 0


def get_git_info():
    """Gitの現在コミット情報を取得"""
    info = {}
    try:
        r = subprocess.run(["git", "log", "-1", "--format=%h|%H|%s|%ai"],
                           capture_output=True, text=True, cwd=ROOT, timeout=5)
        if r.returncode == 0:
            parts = r.stdout.strip().split("|", 3)
            info["short_hash"] = parts[0]
            info["full_hash"] = parts[1]
            info["message"] = parts[2]
            info["date"] = parts[3]

        r = subprocess.run(["git", "rev-list", "--count", "HEAD"],
                           capture_output=True, text=True, cwd=ROOT, timeout=5)
        if r.returncode == 0:
            info["commit_count"] = int(r.stdout.strip())

        r = subprocess.run(["git", "remote", "get-url", "origin"],
                           capture_output=True, text=True, cwd=ROOT, timeout=5)
        if r.returncode == 0:
            info["remote"] = r.stdout.strip()
    except:
        pass
    return info


# ─── Generators ───────────────────────────────────────────

def generate_manifest(src_files, test_files):
    git = get_git_info()
    layers = sorted(set(classify_layer(f) for f in src_files))

    # エントリーポイント検出（main関数）
    entry_points = []
    for f in src_files:
        try:
            with open(f, "r", encoding="utf-8", errors="ignore") as fh:
                if re.search(r'\bint\s+main\s*\(', fh.read()):
                    entry_points.append(os.path.relpath(f, ROOT).replace("\\", "/"))
        except:
            pass

    return {
        "project": "MirageVulkan",
        "generated_at": datetime.datetime.now().isoformat(),
        "commit": git.get("short_hash", "unknown"),
        "commit_full": git.get("full_hash", ""),
        "commit_message": git.get("message", ""),
        "commit_date": git.get("date", ""),
        "commit_count": git.get("commit_count", 0),
        "remote": git.get("remote", ""),
        "src_files": len(src_files),
        "test_count": len(test_files),
        "layers": layers,
        "entry_points": entry_points,
        "devices": [
            {"model": "Npad X1", "resolution": "1200x2000", "android": 13},
            {"model": "A9", "resolution": "800x1340", "android": 15},
        ],
        "cache_files": [
            "project_manifest.json",
            "class_index.json",
            "include_graph.json",
            "thread_map.json",
            "layer_map.json",
            "file_summary.json",
        ],
    }


def generate_class_index(src_files):
    all_classes = []
    for f in src_files:
        layer = classify_layer(f)
        for c in extract_classes(f):
            c["layer"] = layer
            all_classes.append(c)

    by_layer = {}
    for c in all_classes:
        by_layer.setdefault(c["layer"], []).append(c)

    return {
        "generated_at": datetime.datetime.now().isoformat(),
        "total_classes": len([c for c in all_classes if c["kind"] == "class"]),
        "total_structs": len([c for c in all_classes if c["kind"] == "struct"]),
        "by_layer": by_layer,
        "all": all_classes,
    }


def generate_include_graph(hpp_files):
    graph = {}
    for f in hpp_files:
        rel = os.path.relpath(f, SRC_DIR).replace("\\", "/")
        includes = extract_includes(f)
        if includes:
            graph[rel] = includes

    reverse = {}
    for src, deps in graph.items():
        for dep in deps:
            reverse.setdefault(dep, []).append(src)

    most_depended = sorted(reverse.items(), key=lambda x: -len(x[1]))[:10]

    return {
        "generated_at": datetime.datetime.now().isoformat(),
        "total_files": len(graph),
        "forward": graph,
        "reverse_top10": [{"file": f, "depended_by": d, "count": len(d)} for f, d in most_depended],
    }


def generate_thread_map(src_files):
    all_threads = []
    by_file = {}
    for f in src_files:
        threads = extract_threads(f)
        all_threads.extend(threads)
        if threads:
            fname = threads[0]["file"]
            by_file[fname] = threads

    all_mutexes = {}
    for f in src_files:
        rel = os.path.relpath(f, ROOT).replace("\\", "/")
        mutexes = extract_mutexes(f)
        if mutexes:
            all_mutexes[rel] = mutexes

    return {
        "generated_at": datetime.datetime.now().isoformat(),
        "total_thread_points": len(all_threads),
        "total_mutex_files": len(all_mutexes),
        "threads_by_file": by_file,
        "mutexes_by_file": all_mutexes,
        "concurrency_risk": [
            {"file": f, "threads": len(by_file.get(f, [])), "mutexes": len(all_mutexes.get(f, []))}
            for f in set(list(by_file.keys()) + list(all_mutexes.keys()))
            if len(by_file.get(f, [])) > 0 and len(all_mutexes.get(f, [])) > 0
        ],
    }


def generate_layer_map(src_files):
    layers = {}
    for f in src_files:
        rel = os.path.relpath(f, ROOT).replace("\\", "/")
        layer = classify_layer(f)
        total, code = count_lines(f)
        layers.setdefault(layer, []).append({
            "file": rel, "total_lines": total, "code_lines": code,
        })

    stats = {}
    for layer, files in layers.items():
        stats[layer] = {
            "file_count": len(files),
            "total_lines": sum(f["total_lines"] for f in files),
            "code_lines": sum(f["code_lines"] for f in files),
            "files": [f["file"] for f in files],
        }

    return {
        "generated_at": datetime.datetime.now().isoformat(),
        "layers": stats,
        "layer_ranking": sorted(stats.items(), key=lambda x: -x[1]["code_lines"]),
    }


def generate_file_summary(src_files):
    files = []
    for f in src_files:
        rel = os.path.relpath(f, ROOT).replace("\\", "/")
        total, code = count_lines(f)
        classes = extract_classes(f)
        threads = extract_threads(f)
        mutexes = extract_mutexes(f)

        files.append({
            "file": rel,
            "layer": classify_layer(f),
            "total_lines": total,
            "code_lines": code,
            "classes": len(classes),
            "class_names": [c["name"] for c in classes],
            "thread_points": len(threads),
            "mutex_points": len(mutexes),
            "complexity": "high" if (len(threads) > 2 or len(mutexes) > 3 or code > 500)
                          else "medium" if (len(threads) > 0 or len(mutexes) > 0 or code > 200)
                          else "low",
        })

    files.sort(key=lambda x: -x["code_lines"])
    return {
        "generated_at": datetime.datetime.now().isoformat(),
        "total_files": len(files),
        "high_complexity": [f["file"] for f in files if f["complexity"] == "high"],
        "files": files,
    }


# ─── Main ──────────────────────────────────────────────────

def main():
    os.makedirs(INDEX_DIR, exist_ok=True)
    os.makedirs(REVIEW_DIR, exist_ok=True)

    # ソースファイル収集
    src_files = set()
    for ext in ["*.hpp", "*.cpp", "*.h"]:
        src_files.update(glob.glob(os.path.join(SRC_DIR, ext)))
        src_files.update(glob.glob(os.path.join(SRC_DIR, "**", ext), recursive=True))
    src_files = sorted(src_files)

    hpp_files = [f for f in src_files if f.endswith((".hpp", ".h"))]
    test_files = glob.glob(os.path.join(ROOT, "tests", "*.cpp"))

    print("=== MirageVulkan Project Cache Generator ===")
    print(f"C++ソース: {len(src_files)} 件")
    print(f"テスト: {len(test_files)} 件\n")

    generators = [
        ("project_manifest.json", lambda: generate_manifest(src_files, test_files)),
        ("class_index.json", lambda: generate_class_index(src_files)),
        ("include_graph.json", lambda: generate_include_graph(hpp_files)),
        ("thread_map.json", lambda: generate_thread_map(src_files)),
        ("layer_map.json", lambda: generate_layer_map(src_files)),
        ("file_summary.json", lambda: generate_file_summary(src_files)),
    ]

    for name, gen in generators:
        path = os.path.join(INDEX_DIR, name)
        data = gen()
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        size = os.path.getsize(path) / 1024
        print(f"  {name:<25} {size:>6.1f}KB")

    print(f"\n出力先:")
    print(f"  index:        {INDEX_DIR}")
    print(f"  review_cache: {REVIEW_DIR}")
    print(f"\nAIがproject_manifest.jsonを最初に読めばプロジェクト全体を即座に把握可能。")


if __name__ == "__main__":
    main()
