#!/usr/bin/env python3
# ====================================================
# enums.py
# 
# 共通Enum定義 - 重複を排除して一箇所に統一
# ====================================================

from enum import Enum


class DriverState(Enum):
    """ドライバ状態
    
    Values:
        NOT_INSTALLED: ドライバ未インストール
        INSTALLED: WinUSBドライバ正常インストール済み
        INSTALLED_WDI: WDI経由でインストール済み
        PARTIAL: 部分的にインストール（Service != WinUSB）
        ERROR: エラー状態
        NOT_CONNECTED: デバイス未接続
    """
    NOT_INSTALLED = "NOT_INSTALLED"
    INSTALLED = "INSTALLED"
    INSTALLED_WDI = "INSTALLED_WDI"
    PARTIAL = "PARTIAL"
    ERROR = "ERROR"
    NOT_CONNECTED = "NOT_CONNECTED"


class InstallMethod(Enum):
    """ドライバインストール方式
    
    Values:
        PNPUTIL: pnputil /add-driver を使用
        WDI: wdi-simple.exe を使用
        AUTO: 自動選択（WDI優先）
    """
    PNPUTIL = "pnputil"
    WDI = "wdi"
    AUTO = "auto"
