#!/usr/bin/env python3
"""
validate_config.py - config.json のバリデーション

Usage:
    python validate_config.py                    # デフォルトパスで検証
    python validate_config.py path/to/config.json
"""

import json
import sys
import os

DEFAULT_CONFIG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "config.json")

# 必須キーとその型・制約の定義
SCHEMA = {
    "network": {
        "pc_ip": {"type": str, "pattern": r"^\d+\.\d+\.\d+\.\d+$"},
        "video_base_port": {"type": int, "min": 1024, "max": 65535},
        "command_base_port": {"type": int, "min": 1024, "max": 65535},
        "tcp_command_port": {"type": int, "min": 1024, "max": 65535},
    },
    "usb_tether": {
        "android_ip": {"type": str},
        "pc_subnet": {"type": str},
    },
    "adb": {
        "path": {"type": str},
        "device_port": {"type": int, "min": 1, "max": 65535},
        "process_timeout_ms": {"type": int, "min": 100},
        "command_timeout_ms": {"type": int, "min": 100},
    },
    "gui": {
        "window_width": {"type": int, "min": 320},
        "window_height": {"type": int, "min": 240},
        "vsync": {"type": bool},
        "font_size": {"type": (int, float), "min": 6, "max": 72},
    },
    "video": {
        "default_width": {"type": int, "min": 1},
        "default_height": {"type": int, "min": 1},
    },
    "timeouts": {
        "reconnect_init_ms": {"type": int, "min": 0},
        "reconnect_max_ms": {"type": int, "min": 0},
        "usb_timeout_ms": {"type": int, "min": 0},
        "alive_timeout_ms": {"type": int, "min": 0},
        "switch_cooldown_ms": {"type": int, "min": 0},
    },
    "thresholds": {
        "usb_max_latency_ms": {"type": (int, float), "min": 0},
        "usb_recovery_latency_ms": {"type": (int, float), "min": 0},
        "usb_congestion_mbps": {"type": (int, float), "min": 0},
        "rtt_warning_ms": {"type": (int, float), "min": 0},
        "rtt_critical_ms": {"type": (int, float), "min": 0},
    },
    "paths": {
        "shader_dir": {"type": str},
        "log_file": {"type": str},
        "templates_dir": {"type": str},
    },
    "ai": {
        "enabled": {"type": bool},
        "templates_dir": {"type": str},
        "default_threshold": {"type": (int, float), "min": 0.0, "max": 1.0},
    },
    "ocr": {
        "enabled": {"type": bool},
        "language": {"type": str},
    },
}


def validate(config, schema, path=""):
    """再帰的にconfig を検証"""
    errors = []
    warnings = []

    for key, rules in schema.items():
        full_path = f"{path}.{key}" if path else key

        if key not in config:
            errors.append(f"[MISSING] {full_path} が存在しません")
            continue

        value = config[key]

        if isinstance(rules, dict) and "type" not in rules:
            # ネストされたセクション
            if not isinstance(value, dict):
                errors.append(f"[TYPE] {full_path}: objectであるべきです (実際: {type(value).__name__})")
            else:
                e, w = validate(value, rules, full_path)
                errors.extend(e)
                warnings.extend(w)
            continue

        # 型チェック
        expected = rules.get("type")
        if expected and not isinstance(value, expected):
            errors.append(f"[TYPE] {full_path}: {expected} であるべきです (実際: {type(value).__name__} = {value})")
            continue

        # 範囲チェック
        if "min" in rules and isinstance(value, (int, float)):
            if value < rules["min"]:
                errors.append(f"[RANGE] {full_path}: {rules['min']} 以上であるべきです (実際: {value})")

        if "max" in rules and isinstance(value, (int, float)):
            if value > rules["max"]:
                errors.append(f"[RANGE] {full_path}: {rules['max']} 以下であるべきです (実際: {value})")

    # config内の未定義キーを警告
    if isinstance(config, dict):
        for key in config:
            full_path = f"{path}.{key}" if path else key
            if key not in schema:
                warnings.append(f"[UNKNOWN] {full_path}: スキーマに定義されていないキーです")

    return errors, warnings


def check_port_conflicts(config):
    """ポート番号の競合チェック"""
    errors = []
    ports = {}

    def add_port(name, value):
        if value in ports:
            errors.append(f"[CONFLICT] ポート {value} が重複: {ports[value]} と {name}")
        ports[value] = name

    net = config.get("network", {})
    add_port("network.video_base_port", net.get("video_base_port"))
    add_port("network.command_base_port", net.get("command_base_port"))
    add_port("network.tcp_command_port", net.get("tcp_command_port"))

    return errors


def check_logic(config):
    """論理的整合性チェック"""
    warnings = []
    t = config.get("thresholds", {})

    recovery = t.get("usb_recovery_latency_ms", 0)
    max_lat = t.get("usb_max_latency_ms", 0)
    if recovery >= max_lat:
        warnings.append(f"[LOGIC] usb_recovery_latency_ms ({recovery}) >= usb_max_latency_ms ({max_lat}): 復帰閾値は最大を下回るべきです")

    rtt_warn = t.get("rtt_warning_ms", 0)
    rtt_crit = t.get("rtt_critical_ms", 0)
    if rtt_warn >= rtt_crit:
        warnings.append(f"[LOGIC] rtt_warning_ms ({rtt_warn}) >= rtt_critical_ms ({rtt_crit}): warning < critical であるべきです")

    to = config.get("timeouts", {})
    init = to.get("reconnect_init_ms", 0)
    max_r = to.get("reconnect_max_ms", 0)
    if init > max_r:
        warnings.append(f"[LOGIC] reconnect_init_ms ({init}) > reconnect_max_ms ({max_r})")

    return warnings


def main():
    config_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CONFIG

    if not os.path.exists(config_path):
        print(f"[ERROR] ファイルが見つかりません: {config_path}")
        sys.exit(1)

    print(f"=== config.json Validator ===")
    print(f"ファイル: {config_path}\n")

    # JSONパース
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            config = json.load(f)
    except json.JSONDecodeError as e:
        print(f"[FATAL] JSONパースエラー: {e}")
        sys.exit(1)

    # スキーマ検証
    errors, warnings = validate(config, SCHEMA)

    # ポート競合
    errors.extend(check_port_conflicts(config))

    # 論理整合性
    warnings.extend(check_logic(config))

    # 結果表示
    if errors:
        print(f"[ERRORS] {len(errors)} 件:")
        for e in errors:
            print(f"  ✗ {e}")

    if warnings:
        print(f"\n[WARNINGS] {len(warnings)} 件:")
        for w in warnings:
            print(f"  ! {w}")

    if not errors and not warnings:
        print("[OK] 全項目正常 ✓")
        # 設定サマリー
        net = config.get("network", {})
        print(f"\n  PC IP: {net.get('pc_ip')}")
        print(f"  Video port: {net.get('video_base_port')}")
        print(f"  Command port: {net.get('command_base_port')}")
        print(f"  AI: {'ON' if config.get('ai', {}).get('enabled') else 'OFF'}")
        print(f"  OCR: {'ON' if config.get('ocr', {}).get('enabled') else 'OFF'}")
    elif not errors:
        print(f"\n[PASS] エラーなし (警告 {len(warnings)} 件)")
    else:
        print(f"\n[FAIL] エラー {len(errors)} 件")
        sys.exit(1)


if __name__ == "__main__":
    main()
