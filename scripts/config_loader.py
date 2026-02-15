#!/usr/bin/env python3
"""
Config loader for MirageTestKit v2.0
Loads settings from config.json in project root.
"""

import json
import os
from pathlib import Path
from typing import Any, Optional

# Default configuration values
DEFAULT_CONFIG = {
    "network": {
        "pc_ip": "192.168.0.8",
        "video_base_port": 60000,
        "command_base_port": 50000,
        "tcp_command_port": 50100
    },
    "usb_tether": {
        "android_ip": "192.168.42.129",
        "pc_subnet": "192.168.42.0/24"
    },
    "android": {
        "default_mirror_host": "192.168.0.8",
        "default_mirror_port": 60000
    }
}

_config_cache: Optional[dict] = None


def get_project_root() -> Path:
    """Get project root directory (parent of scripts folder)."""
    return Path(__file__).parent.parent


def load_config() -> dict:
    """Load configuration from config.json."""
    global _config_cache

    if _config_cache is not None:
        return _config_cache

    config_path = get_project_root() / "config.json"

    if config_path.exists():
        try:
            with open(config_path, 'r', encoding='utf-8') as f:
                _config_cache = json.load(f)
                return _config_cache
        except (json.JSONDecodeError, IOError) as e:
            print(f"[Config] Warning: Failed to load {config_path}: {e}")

    # Return default config if file doesn't exist or is invalid
    _config_cache = DEFAULT_CONFIG.copy()
    return _config_cache


def get(key: str, default: Any = None) -> Any:
    """
    Get a configuration value by dot-notation key.

    Example:
        get("network.pc_ip") -> "192.168.0.8"
        get("network.video_base_port") -> 60000
    """
    config = load_config()
    keys = key.split('.')
    value = config

    for k in keys:
        if isinstance(value, dict) and k in value:
            value = value[k]
        else:
            return default

    return value


def get_pc_ip() -> str:
    """Get PC IP address for video streaming."""
    return get("network.pc_ip", "192.168.0.8")


def get_video_base_port() -> int:
    """Get base port for video streaming."""
    value = get("network.video_base_port", 60000)
    try:
        port = int(value)
        return max(1, min(65535, port))
    except (ValueError, TypeError):
        return 60000


def get_command_base_port() -> int:
    """Get base port for command communication."""
    value = get("network.command_base_port", 50000)
    try:
        port = int(value)
        return max(1, min(65535, port))
    except (ValueError, TypeError):
        return 50000


def get_android_tether_ip() -> str:
    """Get Android IP when USB tethering is active."""
    return get("usb_tether.android_ip", "192.168.42.129")


def reload_config() -> dict:
    """Force reload configuration from file."""
    global _config_cache
    _config_cache = None
    return load_config()


# Auto-detect local IP if needed
def detect_local_ip() -> str:
    """Detect local IP address (first non-loopback IPv4)."""
    import socket

    # Try multiple external addresses for better compatibility
    external_hosts = [
        ("8.8.8.8", 80),       # Google DNS
        ("1.1.1.1", 80),       # Cloudflare DNS
        ("208.67.222.222", 80), # OpenDNS
    ]

    for host, port in external_hosts:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(1)
            s.connect((host, port))
            ip = s.getsockname()[0]
            s.close()
            if ip and ip != "0.0.0.0":
                return ip
        except (OSError, socket.error, socket.timeout):
            continue

    # Fallback: get hostname-based IP
    try:
        hostname = socket.gethostname()
        ip = socket.gethostbyname(hostname)
        if ip and not ip.startswith("127."):
            return ip
    except socket.error:
        pass

    return "127.0.0.1"


if __name__ == "__main__":
    # Test config loading
    print("MirageTestKit Config Loader")
    print("=" * 40)
    print(f"Project root: {get_project_root()}")
    print(f"PC IP: {get_pc_ip()}")
    print(f"Video base port: {get_video_base_port()}")
    print(f"Command base port: {get_command_base_port()}")
    print(f"Android tether IP: {get_android_tether_ip()}")
    print(f"Detected local IP: {detect_local_ip()}")
