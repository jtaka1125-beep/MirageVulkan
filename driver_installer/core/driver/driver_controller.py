#!/usr/bin/env python3
# ====================================================
# driver_controller.py
# 
# ドライバ制御のバックエンドロジック
# PowerShell + pnputil をラップ
# ====================================================

import subprocess
import json
import logging
import ctypes
from pathlib import Path
from typing import Optional, Dict
from datetime import datetime

from .enums import DriverState

# =====================================================
# Logger Setup
# =====================================================

logging.basicConfig(
    filename="driver_controller.log",
    level=logging.DEBUG,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# =====================================================
# Driver Controller
# =====================================================

class DriverController:
    """WinUSB AOA ドライバ制御"""
    
    DEVICE_VID = "18D1"
    DEVICE_PID = "2D01"
    DEVICE_MI = "00"
    HWID = f"USB\\VID_{DEVICE_VID}&PID_{DEVICE_PID}&MI_{DEVICE_MI}"
    
    def __init__(self):
        self.logger = logger
        self.wdi_exe = Path("wdi-simple.exe")
        self.driver_flag = Path(".driver_installed")
        self.backup_file = Path("driver_backup.txt")
    
    # ===== Admin Check =====
    
    @staticmethod
    def check_admin() -> bool:
        """管理者権限を確認"""
        try:
            return ctypes.windll.shell32.IsUserAnAdmin() != 0
        except Exception:
            return False
    
    # ===== WDI / pnputil Detection =====
    
    def check_wdi_available(self) -> bool:
        """wdi-simple.exe が利用可能か確認"""
        if self.wdi_exe.exists():
            return True
        
        try:
            result = subprocess.run(
                ['where', 'wdi-simple.exe'],
                capture_output=True,
                text=True,
                timeout=5
            )
            return result.returncode == 0
        except Exception:
            return False
    
    def check_pnputil_available(self) -> bool:
        """pnputil が利用可能か確認"""
        try:
            result = subprocess.run(
                ['pnputil', '/?'],
                capture_output=True,
                text=True,
                timeout=5
            )
            return result.returncode == 0
        except Exception:
            return False
    
    # ===== Device Detection (PowerShell) =====
    
    def find_aoa_device(self) -> Optional[str]:
        """AOA デバイスの InstanceID を検出（PowerShell）"""
        self.logger.info("Finding AOA device via PowerShell...")
        
        ps_script = '''
$devices = Get-PnpDevice -PresentOnly | Where-Object { 
    $_.InstanceId -like "*VID_18D1*PID_2D01*MI_00*" 
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
                instance_id = result.stdout.strip()
                self.logger.info(f"Device found: {instance_id}")
                return instance_id
            else:
                self.logger.warning("Device not found")
                return None
        
        except Exception as e:
            self.logger.error(f"PowerShell error: {e}")
            return None
    
    def get_device_info(self, instance_id: str) -> Optional[Dict]:
        """デバイス情報を取得"""
        self.logger.info(f"Getting device info for {instance_id}...")
        
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
        status = $dev.Status
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
                info = json.loads(result.stdout)
                self.logger.info(f"Device info: {info}")
                return info
            else:
                return None
        
        except Exception as e:
            self.logger.error(f"Device info error: {e}")
            return None
    
    # ===== Driver Installation =====
    
    def install_driver(self) -> bool:
        """ドライバをインストール"""
        self.logger.info("Installing driver...")
        
        # WDI が利用可能ならそちらを優先
        if self.check_wdi_available():
            self.logger.info("Using WDI (wdi-simple.exe)")
            return self.install_via_wdi()
        else:
            self.logger.info("Using pnputil (fallback)")
            return self.install_via_pnputil()
    
    def install_via_wdi(self) -> bool:
        """WDI (wdi-simple.exe) でインストール"""
        self.logger.info("Installing via wdi-simple...")
        
        try:
            cmd = [
                'wdi-simple.exe' if not self.wdi_exe.exists() else str(self.wdi_exe),
                '--vid', '0x18D1',
                '--pid', '0x2D01',
                '--iid', '0',
                '--type', '0'
            ]
            
            self.logger.info(f"Running: {' '.join(cmd)}")
            
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=30
            )
            
            if result.returncode == 0:
                self.logger.info("WDI installation succeeded")
                self.create_flag_file("wdi-simple")
                return True
            else:
                self.logger.error(f"WDI installation failed: {result.stderr}")
                return False
        
        except Exception as e:
            self.logger.error(f"WDI installation error: {e}")
            return False
    
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
            
            if result.returncode == 0:
                self.logger.info("pnputil installation succeeded")
                self.create_flag_file("pnputil")
                return True
            else:
                self.logger.error(f"pnputil installation failed: {result.stderr}")
                return False
        
        except Exception as e:
            self.logger.error(f"pnputil installation error: {e}")
            return False
    
    # ===== Driver Verification =====
    
    def check_driver_state(self) -> DriverState:
        """ドライバ状態を確認"""
        self.logger.info("Checking driver state...")
        
        instance_id = self.find_aoa_device()
        
        if not instance_id:
            self.logger.warning("Device not found")
            return DriverState.NOT_CONNECTED
        
        info = self.get_device_info(instance_id)
        
        if not info:
            self.logger.warning("Could not get device info")
            return DriverState.PARTIAL
        
        service = info.get('service', '')
        
        if service == 'WinUSB':
            self.logger.info("Driver verified: Service=WinUSB")
            return DriverState.INSTALLED
        else:
            self.logger.warning(f"Driver service is {service} (expected WinUSB)")
            return DriverState.PARTIAL
    
    # ===== Rollback =====
    
    def rollback_driver(self) -> bool:
        """ドライバをロールバック（2段階）"""
        self.logger.info("Rolling back driver (2-stage)...")
        
        # Stage 1: Device removal
        instance_id = self.find_aoa_device()
        
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
                    self.logger.info("Device removed")
                else:
                    self.logger.warning(f"Device removal status: {result.returncode}")
            
            except Exception as e:
                self.logger.warning(f"Device removal error: {e}")
        
        # Stage 2: OEM driver deletion
        if instance_id:
            oem_driver = self.find_oem_driver(instance_id)
            
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
                        self.logger.info(f"Driver {oem_driver} deleted")
                    else:
                        self.logger.warning(f"Driver deletion status: {result.returncode}")
                
                except Exception as e:
                    self.logger.warning(f"Driver deletion error: {e}")
        
        # Remove flag
        if self.driver_flag.exists():
            self.driver_flag.unlink()
        
        self.logger.info("Rollback completed")
        return True
    
    def find_oem_driver(self, instance_id: str) -> Optional[str]:
        """OEM ドライバを見つける"""
        self.logger.info(f"Finding OEM driver for {instance_id}...")
        
        try:
            result = subprocess.run(
                ['pnputil', '/enum-drivers'],
                capture_output=True,
                text=True,
                timeout=10
            )
            
            # 簡易検索: VID_18D1 を含む oem*.inf を見つける
            for line in result.stdout.split('\n'):
                if 'VID_18D1' in line or 'PID_2D01' in line:
                    # oem*.inf を抽出
                    if 'oem' in line:
                        parts = line.split()
                        for part in parts:
                            if part.startswith('oem') and part.endswith('.inf'):
                                self.logger.info(f"Found OEM driver: {part}")
                                return part
            
            self.logger.warning("No OEM driver found")
            return None
        
        except Exception as e:
            self.logger.error(f"OEM driver search error: {e}")
            return None
    
    # ===== Support Methods =====
    
    def backup_drivers(self) -> bool:
        """ドライバ情報をバックアップ"""
        if self.backup_file.exists():
            return True
        
        try:
            result = subprocess.run(
                ['pnputil', '/enum-drivers'],
                capture_output=True,
                text=True,
                timeout=5
            )
            
            with open(self.backup_file, 'w') as f:
                f.write(result.stdout)
            
            self.logger.info(f"Backup created: {self.backup_file}")
            return True
        
        except Exception as e:
            self.logger.error(f"Backup error: {e}")
            return False
    
    def create_flag_file(self, method: str):
        """インストール完了フラグを作成"""
        try:
            with open(self.driver_flag, 'w') as f:
                f.write(f"Installed: {datetime.now()}\n")
                f.write(f"Method: {method}\n")
                f.write(f"HWID: {self.HWID}\n")
                f.write("Status: OK\n")
            
            self.logger.info("Flag file created")
        
        except Exception as e:
            self.logger.error(f"Flag file creation error: {e}")
    
    def get_status_summary(self) -> Dict:
        """ステータスサマリーを取得"""
        instance_id = self.find_aoa_device()
        
        if not instance_id:
            return {
                "device_connected": False,
                "driver_installed": False,
                "service_ok": False
            }
        
        info = self.get_device_info(instance_id)
        
        if not info:
            return {
                "device_connected": True,
                "driver_installed": False,
                "service_ok": False
            }
        
        return {
            "device_connected": True,
            "driver_installed": True,
            "service_ok": info.get('service') == 'WinUSB',
            "service": info.get('service'),
            "driver": info.get('driver'),
            "provider": info.get('provider'),
            "version": info.get('version')
        }
