#!/usr/bin/env python3
"""
generate_project_cache.py - プロジェクト構造キャッシュ生成

C++/Python/Kotlinソースを解析し、AIが高速に全体把握できる
構造化JSONを生成する。

出力先: C:\\MirageWork\\mcp-server\\workspace\\

生成ファイル:
  class_index.json      - 全クラス・構造体一覧と役割
  include_graph.json    - ヘッダ依存関係
  thread_map.json       - スレッド生成箇所
  layer_map.json        - 層別ファイル分類
  file_summary.json     - ファイル別行数・複雑度
  project_overview.json - プロジェクト全体統計

Usage:
    python generate_project_cache.py
"""

import os
import re
import json
import glob
import datetime

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC_DIR = os.path.join(ROOT, "src")
WORKSPACE_DIR = r"C:\MirageWork\mcp-server\workspace"

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
    """ファイル名から層を判定"""
    base = os.path.splitext(os.path.basename(filename))[0].lower()
    for layer, keywords in LAYER_KEYWORDS.items():
        for kw in keywords:
            if kw in base:
                return layer
    return "other"


def extract_classes(filepath):
    """C++ファイルからクラス/構造体を抽出"""
    classes = []
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            for i, line in enumerate(f, 1):
                # class Foo { or class Foo :
                m = re.match(r'^(class|struct)\s+(\w+)', line)
                if m:
                    kind = m.group(1)
                    name = m.group(2)
                    # 前方宣言を除外
                    if ";" in line and "{" not in line:
                        continue
                    classes.append({
                        "name": name,
                        "kind": kind,
                        "line": i,
                        "file": os.path.relpath(filepath, ROOT).replace("\\", "/"),
                    })
    except:
        pass
    return classes


def extract_includes(filepath):
    """#include "xxx" を抽出"""
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
    """スレッド生成箇所を抽出"""
    threads = []
    patterns = [
        (r'std::thread\s*\(', "std::thread"),
        (r'std::async\s*\(', "std::async"),
        (r'std::thread\s+\w+', "std::thread member"),
    ]
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            for i, line in enumerate(f, 1):
                for pat, kind in patterns:
                    if re.search(pat, line):
                        threads.append({
                            "line": i,
                            "kind": kind,
                            "code": line.strip()[:100],
                            "file": os.path.relpath(filepath, ROOT).replace("\\", "/"),
                        })
    except:
        pass
    return threads


def extract_mutexes(filepath):
    """mutex/lock使用箇所を抽出"""
    mutexes = []
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            for i, line in enumerate(f, 1):
                if re.search(r'std::mutex|std::lock_guard|std::unique_lock|std::shared_mutex', line):
                    mutexes.append({
                        "line": i,
                        "code": line.strip()[:100],
                    })
    except:
        pass
    return mutexes


def count_lines(filepath):
    """ファイル行数カウント"""
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.readlines()
            total = len(lines)
            code = len([l for l in lines if l.strip() and not l.strip().startswith("//")])
            return total, code
    except:
        return 0, 0


def generate_class_index(src_files):
    """class_index.json 生成"""
    all_classes = []
    for f in src_files:
        classes = extract_classes(f)
        layer = classify_layer(f)
        for c in classes:
            c["layer"] = layer
            all_classes.append(c)

    # 層別にグループ化
    by_layer = {}
    for c in all_classes:
        layer = c["layer"]
        if layer not in by_layer:
            by_layer[layer] = []
        by_layer[layer].append(c)

    return {
        "generated_at": datetime.datetime.now().isoformat(),
        "total_classes": len([c for c in all_classes if c["kind"] == "class"]),
        "total_structs": len([c for c in all_classes if c["kind"] == "struct"]),
        "by_layer": by_layer,
        "all": all_classes,
    }


def generate_include_graph(hpp_files):
    """include_graph.json 生成"""
    graph = {}
    for f in hpp_files:
        rel = os.path.relpath(f, SRC_DIR).replace("\\", "/")
        includes = extract_includes(f)
        if includes:
            graph[rel] = includes

    # 逆引き（誰から参照されているか）
    reverse = {}
    for src, deps in graph.items():
        for dep in deps:
            if dep not in reverse:
                reverse[dep] = []
            reverse[dep].append(src)

    # 最も参照されているファイル
    most_depended = sorted(reverse.items(), key=lambda x: -len(x[1]))[:10]

    return {
        "generated_at": datetime.datetime.now().isoformat(),
        "total_files": len(graph),
        "forward": graph,
        "reverse_top10": [{"file": f, "depended_by": d, "count": len(d)} for f, d in most_depended],
    }


def generate_thread_map(src_files):
    """thread_map.json 生成"""
    all_threads = []
    for f in src_files:
        threads = extract_threads(f)
        all_threads.extend(threads)

    # ファイル別集計
    by_file = {}
    for t in all_threads:
        fname = t["file"]
        if fname not in by_file:
            by_file[fname] = []
        by_file[fname].append(t)

    # mutex情報
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
    }


def generate_layer_map(src_files):
    """layer_map.json 生成"""
    layers = {}
    for f in src_files:
        rel = os.path.relpath(f, ROOT).replace("\\", "/")
        layer = classify_layer(f)
        if layer not in layers:
            layers[layer] = []
        total, code = count_lines(f)
        layers[layer].append({
            "file": rel,
            "total_lines": total,
            "code_lines": code,
        })

    # 層別統計
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
    }


def generate_file_summary(src_files):
    """file_summary.json 生成"""
    files = []
    for f in src_files:
        rel = os.path.relpath(f, ROOT).replace("\\", "/")
        total, code = count_lines(f)
        layer = classify_layer(f)
        classes = extract_classes(f)
        threads = extract_threads(f)
        mutexes = extract_mutexes(f)

        files.append({
            "file": rel,
            "layer": layer,
            "total_lines": total,
            "code_lines": code,
            "classes": len(classes),
            "class_names": [c["name"] for c in classes],
            "thread_points": len(threads),
            "mutex_points": len(mutexes),
            "complexity_hint": "high" if (len(threads) > 2 or len(mutexes) > 3 or code > 500) else
                               "medium" if (len(threads) > 0 or len(mutexes) > 0 or code > 200) else "low",
        })

    files.sort(key=lambda x: -x["code_lines"])
    return {
        "generated_at": datetime.datetime.now().isoformat(),
        "total_files": len(files),
        "files": files,
    }


def generate_project_overview(src_files, all_files):
    """project_overview.json 生成"""
    total_lines = 0
    total_code = 0
    for f in src_files:
        t, c = count_lines(f)
        total_lines += t
        total_code += c

    hpp_count = len([f for f in src_files if f.endswith(".hpp")])
    cpp_count = len([f for f in src_files if f.endswith(".cpp")])

    # Android
    kt_files = glob.glob(os.path.join(ROOT, "android", "**", "*.kt"), recursive=True)
    java_files = glob.glob(os.path.join(ROOT, "android", "**", "*.java"), recursive=True)

    # Scripts
    py_files = glob.glob(os.path.join(ROOT, "scripts", "*.py"))
    bat_files = glob.glob(os.path.join(ROOT, "scripts", "*.bat"))

    # Shaders
    comp_files = glob.glob(os.path.join(ROOT, "shaders", "*.comp"))

    # Tests
    test_files = glob.glob(os.path.join(ROOT, "tests", "*.cpp"))

    return {
        "generated_at": datetime.datetime.now().isoformat(),
        "project": "MirageVulkan",
        "cpp": {
            "hpp_files": hpp_count,
            "cpp_files": cpp_count,
            "total_lines": total_lines,
            "code_lines": total_code,
        },
        "android": {
            "kotlin_files": len(kt_files),
            "java_files": len(java_files),
            "modules": ["app (com.mirage.android)", "accessory (com.mirage.accessory)",
                        "capture (com.mirage.capture)"],
        },
        "scripts": {
            "python": len(py_files),
            "batch": len(bat_files),
        },
        "shaders": len(comp_files),
        "tests": len(test_files),
        "devices": [
            {"model": "Npad X1", "resolution": "1200x2000", "android": 13, "sdk": 33},
            {"model": "A9", "resolution": "800x1340", "android": 15, "sdk": 35},
        ],
    }


def main():
    os.makedirs(WORKSPACE_DIR, exist_ok=True)

    # ソースファイル収集
    src_files = []
    for ext in ["*.hpp", "*.cpp", "*.h"]:
        src_files.extend(glob.glob(os.path.join(SRC_DIR, ext)))
        src_files.extend(glob.glob(os.path.join(SRC_DIR, "**", ext), recursive=True))
    src_files = list(set(src_files))

    hpp_files = [f for f in src_files if f.endswith((".hpp", ".h"))]

    all_project_files = glob.glob(os.path.join(ROOT, "**", "*"), recursive=True)

    print("=== MirageVulkan Project Cache Generator ===")
    print(f"ソースファイル: {len(src_files)} 件\n")

    # 各JSON生成
    generators = [
        ("class_index.json", lambda: generate_class_index(src_files)),
        ("include_graph.json", lambda: generate_include_graph(hpp_files)),
        ("thread_map.json", lambda: generate_thread_map(src_files)),
        ("layer_map.json", lambda: generate_layer_map(src_files)),
        ("file_summary.json", lambda: generate_file_summary(src_files)),
        ("project_overview.json", lambda: generate_project_overview(src_files, all_project_files)),
    ]

    for name, gen in generators:
        path = os.path.join(WORKSPACE_DIR, name)
        data = gen()
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        size = os.path.getsize(path) / 1024
        print(f"  {name:<25} {size:>6.1f}KB")

    print(f"\n出力先: {WORKSPACE_DIR}")
    print("これらのJSONを読めばプロジェクト全体を高速に再構築可能。")


if __name__ == "__main__":
    main()
