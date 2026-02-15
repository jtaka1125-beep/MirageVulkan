#!/usr/bin/env python3
# ====================================================
# setup_orchestrator.py
# 
# 修正版: 署名対応 + InstanceID堅牢化 + rollback2段対応
# 
# 修正点:
# 1. InstanceID取得を PowerShell (Get-PnpDevice) に統一
# 2. --wdi-install の呼び出し整合修正
# 3. rollback を2段化（remove-device + delete-driver）
# 4. 署名・証明書ウォーニング追加
# ====================================================

import os
import sys
import subprocess
import io

# Fix cp932 encoding for emoji/unicode
if sys.stdout.encoding != 'utf-8':
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')
import json
import logging
from datetime import datetime
from pathlib import Path
from typing import Dict, Optional, List, Tuple

from .enums import DriverState, InstallMethod

# =====================================================
# Visual Logger
# =====================================================

class VisualLogger:
    """ビジュアルロギング - カラー出力付き"""
    
    COLORS = {
        'RESET': '\033[0m',
        'BOLD': '\033[1m',
        'DIM': '\033[2m',
        'GREEN': '\033[92m',
        'YELLOW': '\033[93m',
        'RED': '\033[91m',
        'BLUE': '\033[94m',
        'CYAN': '\033[96m',
    }
    
    def __init__(self, name: str, log_file: str = None):
        self.name = name
        self.log_file = log_file
        
        if log_file:
            logging.basicConfig(
                filename=log_file,
                level=logging.DEBUG,
                format='%(asctime)s - %(levelname)s - %(message)s'
            )
    
    def info(self, message: str):
        print(f"{self.COLORS['CYAN']}[{self.name}] {message}{self.COLORS['RESET']}")
        if self.log_file:
            logging.info(message)
    
    def success(self, message: str):
        print(f"{self.COLORS['GREEN']}[OK] {message}{self.COLORS['RESET']}")
        if self.log_file:
            logging.info(f"SUCCESS: {message}")
    
    def warning(self, message: str):
        print(f"{self.COLORS['YELLOW']}[WARN] {message}{self.COLORS['RESET']}")
        if self.log_file:
            logging.warning(message)
    
    def error(self, message: str):
        print(f"{self.COLORS['RED']}[ERROR] {message}{self.COLORS['RESET']}")
        if self.log_file:
            logging.error(message)
    
    def display_status(self, status: Dict[str, bool]):
        """ステータス表示"""
        print("\n" + "="*60)
        print(f"{self.COLORS['BOLD']}Device & Driver Status{self.COLORS['RESET']}")
        print("="*60)
        
        for key, value in status.items():
            symbol = f"{self.COLORS['GREEN']}✓{self.COLORS['RESET']}" if value else f"{self.COLORS['RED']}✗{self.COLORS['RESET']}"
            print(f"  {symbol} {key}")
        
        print("="*60 + "\n")

# =====================================================
# PowerShell Helper
# =====================================================

class PowerShellHelper:
    """PowerShell 呼び出しのヘルパー"""
    
    @staticmethod
    def get_aoa_instanceid() -> Optional[str]:
        r"""
        AOA デバイスの InstanceID を取得（PowerShell経由）
        
        Returns:
            str: InstanceID (e.g., "USB\VID_18D1&PID_2D01&MI_00\123456")
            None: デバイス未検出
        """
        ps_script = '''
$devices = Get-PnpDevice | Where-Object { 
    $_.InstanceId -like "*VID_18D1*PID_2D01*" 
}

if ($devices) {
    $dev = $devices | Select-Object -First 1
    Write-Output $dev.InstanceId
    exit 0
} else {
    exit 1
}
'''
        
        try:
            result = subprocess.run(
                ['powershell', '-NoProfile', '-Command', ps_script],
                capture_output=True,
                text=True,
                timeout=10
            )
            
            if result.returncode == 0 and result.stdout.strip():
                return result.stdout.strip()
            else:
                return None
        
        except Exception as e:
            print(f"[PowerShell Error] {e}")
            return None
    
    @staticmethod
    def get_driver_info(instance_id: str) -> Optional[Dict]:
        """
        ドライバ情報を取得（Service, DriverInf, Provider）
        
        Args:
            instance_id: InstanceID
        
        Returns:
            dict: {service, driver_inf, provider, version}
            None: 情報取得失敗
        """
        ps_script = f'''
$instanceId = "{instance_id}"
$dev = Get-PnpDevice -PresentOnly | Where-Object {{ $_.InstanceId -eq $instanceId }}

if ($dev) {{
    $service = (Get-PnpDeviceProperty -InstanceId $instanceId -KeyName "DEVPKEY_Device_Service" -ErrorAction SilentlyContinue).Data
    $driver = (Get-PnpDeviceProperty -InstanceId $instanceId -KeyName "DEVPKEY_Device_DriverInfPath" -ErrorAction SilentlyContinue).Data
    $provider = (Get-PnpDeviceProperty -InstanceId $instanceId -KeyName "DEVPKEY_Device_DriverProvider" -ErrorAction SilentlyContinue).Data
    $version = (Get-PnpDeviceProperty -InstanceId $instanceId -KeyName "DEVPKEY_Device_DriverVersion" -ErrorAction SilentlyContinue).Data
    
    $output = @{{
        service = $service
        driver = $driver
        provider = $provider
        version = $version
    }}
    
    ConvertTo-Json $output
    exit 0
}} else {{
    exit 1
}}
'''
        
        try:
            result = subprocess.run(
                ['powershell', '-NoProfile', '-Command', ps_script],
                capture_output=True,
                text=True,
                timeout=10
            )
            
            if result.returncode == 0 and result.stdout.strip():
                return json.loads(result.stdout)
            else:
                return None
        
        except Exception as e:
            print(f"[PowerShell Error] {e}")
            return None
    
    @staticmethod
    def find_oem_driver(instance_id: str) -> Optional[str]:
        """
        InstanceID に対応する OEM ドライバを見つける
        
        Returns:
            str: "oemXX.inf" 形式
            None: 見つからない
        """
        ps_script = f'''
$instanceId = "{instance_id}"
$drivers = pnputil /enum-drivers 2>$null

if ($drivers -match "oem\\d+\\.inf") {{
    # OEM INF を一覧化
    $driverList = @()
    $currentOem = $null
    
    $drivers -split "`n" | ForEach-Object {{
        if ($_ -match "Published Name\\s*:\\s*(oem\\d+\\.inf)") {{
            $currentOem = $matches[1]
        }}
        if ($currentOem -and $_ -match $instanceId) {{
            Write-Output $currentOem
            exit 0
        }}
    }}
}}

exit 1
'''
        
        try:
            result = subprocess.run(
                ['powershell', '-NoProfile', '-Command', ps_script],
                capture_output=True,
                text=True,
                timeout=10
            )
            
            if result.returncode == 0 and result.stdout.strip():
                return result.stdout.strip()
            else:
                return None
        
        except Exception as e:
            print(f"[PowerShell Error] {e}")
            return None

# =====================================================
# Driver Setup Orchestrator (Fixed)
# =====================================================

class DriverSetupOrchestratorFixed:
    """修正版 - 署名対応 + InstanceID堅牢化 + rollback2段"""
    
    # Device Constants
    DEVICE_VID = "18D1"
    DEVICE_PID = "2D01"
    DEVICE_MI = "00"
    HWID = f"USB\\VID_{DEVICE_VID}&PID_{DEVICE_PID}"  # MI_00 optional
    
    def __init__(self):
        self.logger = VisualLogger("DriverSetupWDI-Fixed", "driver_setup_fixed.log")
        self.driver_flag = Path(".driver_installed")
        self.backup_file = Path("driver_backup.txt")
        self.wdi_exe = Path(__file__).parent.parent.parent / "tools" / "wdi" / "wdi-simple.exe"
        self.pshell = PowerShellHelper()
        
        # インストール方式を自動判定
        self.install_method = InstallMethod.WDI if self.wdi_exe.exists() else InstallMethod.PNPUTIL
        
        if self.install_method == InstallMethod.WDI:
            self.logger.success("wdi-simple.exe detected - WDI mode enabled")
        else:
            self.logger.info("wdi-simple.exe not found - Using pnputil fallback")
        
        # 署名ウォーニング
        self.show_signature_notice()
    
    # ===== Signature Notice =====
    
    def show_signature_notice(self):
        """署名の運用ルールを表示"""
        print("\n" + "="*60)
        print("⚠️  SIGNATURE OPERATIONAL NOTICE")
        print("="*60)
        print("""
このツールは以下の署名状態を想定しています:

[pnputil方式]:
  - テストモード環境
  - または個人・社内限定デプロイ
  
[WDI方式]:
  - 証明書が TrustedPublisher に登録されている
  - または テストモード環境

詳細: SIGNATURE_OPERATIONAL_GUIDE.md を参照

""")
        print("="*60 + "\n")
    
    # ===== Device Detection (PowerShell) =====
    
    def find_aoa_device_instanceid(self) -> Optional[str]:
        """AOA デバイスの InstanceID を取得（PowerShell経由）"""
        self.logger.info("Finding AOA device InstanceID via PowerShell...")
        
        instance_id = self.pshell.get_aoa_instanceid()
        
        if instance_id:
            self.logger.success(f"InstanceID: {instance_id}")
            return instance_id
        else:
            self.logger.warning("InstanceID not found")
            return None
    
    def check_aoa_device_connected(self) -> bool:
        """AOA デバイス接続確認"""
        self.logger.info("Checking for AOA device...")
        
        instance_id = self.find_aoa_device_instanceid()
        return instance_id is not None
    
    def verify_service_is_winusb(self, instance_id: str) -> bool:
        """Service が WinUSB であることを確認"""
        self.logger.info("Verifying Service=WinUSB...")
        
        info = self.pshell.get_driver_info(instance_id)
        
        if not info:
            self.logger.warning("Could not retrieve driver info")
            return False
        
        service = info.get('service', '')
        
        if service == 'WinUSB':
            self.logger.success(f"Service verified: {service}")
            return True
        else:
            self.logger.warning(f"Service is '{service}' (expected 'WinUSB')")
            return False
    
    # ===== Installation Methods =====
    
    def install_via_pnputil(self) -> bool:
        """pnputil でインストール"""
        self.logger.info("Installing via pnputil...")
        
        if not Path("android_accessory_interface.inf").exists():
            self.logger.error("INF file not found")
            return False
        
        try:
            result = subprocess.run(
                ['pnputil', '/add-driver', 
                 'android_accessory_interface.inf', '/install'],
                capture_output=True,
                text=True,
                timeout=30
            )
            
            if result.returncode != 0:
                self.logger.error(f"pnputil failed: {result.stderr}")
                return False
            
            self.logger.success("pnputil installation completed")
            return True
        
        except Exception as e:
            self.logger.error(f"Installation error: {e}")
            return False
    
    def install_via_wdi(self) -> bool:
        """wdi-simple.exe でインストール"""
        self.logger.info("Installing via wdi-simple...")
        
        try:
            cmd = [
                str(self.wdi_exe),
                '--vid', '0x18D1',
                '--pid', '0x2D01',
                '--iid', '0',
                '--type', '0'
            ]
            
            self.logger.info(f"Command: {' '.join(cmd)}")
            
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=30
            )
            
            if result.returncode != 0:
                self.logger.error(f"wdi-simple failed: {result.stderr}")
                return False
            
            self.logger.success("wdi-simple installation completed")
            return True
        
        except Exception as e:
            self.logger.error(f"Installation error: {e}")
            return False
    
    def install_driver_interactive(self) -> bool:
        """対話的にドライバインストール"""
        self.logger.info("Starting interactive driver installation...")
        
        if not self.check_aoa_device_connected():
            self.logger.error("AOA device not connected")
            return False
        
        # バックアップ
        self.backup_drivers()
        
        # インストール方式選択
        if self.install_method == InstallMethod.WDI:
            success = self.install_via_wdi()
        else:
            success = self.install_via_pnputil()
        
        if not success:
            self.logger.error("Installation failed")
            return False
        
        # 検証
        import time
        time.sleep(2)
        
        instance_id = self.find_aoa_device_instanceid()
        if instance_id and self.verify_service_is_winusb(instance_id):
            # フラグ作成
            with open(self.driver_flag, 'w') as f:
                f.write(f"Installed: {datetime.now()}\n")
                f.write(f"Method: {'wdi-simple' if self.install_method == InstallMethod.WDI else 'pnputil'}\n")
                f.write(f"HWID: {self.HWID}\n")
                f.write(f"InstanceID: {instance_id}\n")
                f.write("Status: OK\n")
            
            self.logger.success("Installation completed and verified")
            return True
        else:
            self.logger.warning("Installation completed but verification inconclusive")
            return False
    
    # ===== Rollback (2-stage) =====
    
    def rollback_driver(self) -> bool:
        """
        ドライバを完全にロールバック（2段階）
        
        Stage 1: pnputil /remove-device (InstanceID)
        Stage 2: pnputil /delete-driver oemXX.inf /uninstall /force
        """
        self.logger.warning("Rolling back driver (2-stage)...")
        
        # Stage 1: InstanceID で削除
        instance_id = self.find_aoa_device_instanceid()
        
        if instance_id:
            self.logger.info(f"Stage 1: Removing device {instance_id}...")
            
            try:
                result = subprocess.run(
                    ['pnputil', '/remove-device', instance_id],
                    capture_output=True,
                    text=True,
                    timeout=10
                )
                
                if result.returncode == 0:
                    self.logger.success("Device removed")
                else:
                    self.logger.warning(f"Device removal returned: {result.returncode}")
            
            except Exception as e:
                self.logger.warning(f"Device removal error: {e}")
        else:
            self.logger.warning("Could not find InstanceID (device may be disconnected)")
        
        # Stage 2: OEM ドライバを削除
        if instance_id:
            oem_driver = self.pshell.find_oem_driver(instance_id)
            
            if oem_driver:
                self.logger.info(f"Stage 2: Deleting driver {oem_driver}...")
                
                try:
                    result = subprocess.run(
                        ['pnputil', '/delete-driver', oem_driver, '/uninstall', '/force'],
                        capture_output=True,
                        text=True,
                        timeout=10
                    )
                    
                    if result.returncode == 0:
                        self.logger.success(f"Driver {oem_driver} deleted")
                    else:
                        self.logger.warning(f"Driver deletion returned: {result.returncode}")
                
                except Exception as e:
                    self.logger.warning(f"Driver deletion error: {e}")
            else:
                self.logger.info("No OEM driver found (may already be removed)")
        
        # フラグ削除
        if self.driver_flag.exists():
            self.driver_flag.unlink()
        
        self.logger.success("Rollback completed (2-stage)")
        return True
    
    # ===== Support Methods =====
    
    def backup_drivers(self):
        """ドライバ情報をバックアップ"""
        if self.backup_file.exists():
            return
        
        try:
            result = subprocess.run(
                ['pnputil', '/enum-drivers'],
                capture_output=True,
                text=True,
                timeout=5
            )
            
            with open(self.backup_file, 'w') as f:
                f.write(result.stdout)
            
            self.logger.success(f"Backup created: {self.backup_file}")
        except Exception as e:
            self.logger.warning(f"Backup failed: {e}")
    
    def show_status(self):
        """ステータス表示"""
        instance_id = self.find_aoa_device_instanceid()
        
        status = {
            "AOA Device Connected": instance_id is not None,
            "WinUSB Service OK": instance_id and self.verify_service_is_winusb(instance_id),
            "Driver Flag Exists": self.driver_flag.exists(),
            "WDI Mode Available": self.wdi_exe.exists(),
        }
        
        self.logger.display_status(status)
        
        if instance_id:
            info = self.pshell.get_driver_info(instance_id)
            if info:
                print("\nDriver Details:")
                print(f"  Service:  {info.get('service', 'N/A')}")
                print(f"  Provider: {info.get('provider', 'N/A')}")
                print(f"  Version:  {info.get('version', 'N/A')}")
                print(f"  Driver:   {info.get('driver', 'N/A')}")
                print()
    
    # ===== Setup Wizard =====
    
    def run_wizard(self):
        """セットアップウィザード"""
        print("\n" + "="*60)
        print("MirageSystem v2 - Driver Setup Wizard (Fixed)")
        print("="*60 + "\n")
        
        # Step 1: デバイス確認
        if not self.check_aoa_device_connected():
            self.logger.error("AOA device not connected")
            print("\nPlease:")
            print("  1. Connect Android device via USB")
            print("  2. Enable AOA mode")
            print("  3. Run wizard again")
            return
        
        # Step 2: インストール
        if not self.install_driver_interactive():
            self.logger.error("Installation failed")
            return
        
        # Step 3: 確認
        self.show_status()
        
        print("\n✓ Setup completed successfully")

# =====================================================
# Main Entry Point
# =====================================================

def main():
    """メインエントリーポイント"""
    
    if len(sys.argv) > 1:
        orchestrator = DriverSetupOrchestratorFixed()
        
        if sys.argv[1] == '--check':
            orchestrator.show_status()
        elif sys.argv[1] == '--install':
            orchestrator.install_driver_interactive()
        elif sys.argv[1] == '--rollback':
            orchestrator.rollback_driver()
        elif sys.argv[1] == '--wizard':
            orchestrator.run_wizard()
        else:
            print("Usage:")
            print("  python setup_orchestrator_v2_wdi_fixed.py [option]")
            print("\nOptions:")
            print("  --check      Check driver status")
            print("  --install    Install driver")
            print("  --rollback   Rollback driver (2-stage)")
            print("  --wizard     Interactive wizard")
            return 1
    else:
        orchestrator = DriverSetupOrchestratorFixed()
        orchestrator.run_wizard()
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
