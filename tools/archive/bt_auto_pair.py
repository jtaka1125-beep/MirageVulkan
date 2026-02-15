#!/usr/bin/env python3
"""
Bluetooth Auto-Pairing - PC + Android simultaneous automation
1. Android: enable discoverable mode
2. PC: WinRT PairAsync (background)  
3. Android: monitor & auto-accept pairing dialog (foreground)
"""
import subprocess
import threading
import time
import re
import sys

class BluetoothAutoPair:
    def __init__(self, adb_serial: str, bt_mac: str):
        self.serial = adb_serial
        self.bt_mac = bt_mac
        self.pair_result = None
        
    def adb(self, *args, timeout=10):
        cmd = ["adb", "-s", self.serial] + list(args)
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            return r.stdout.strip()
        except:
            return ""
    
    def step1_discoverable(self):
        """Android: enable discoverable mode"""
        print(f"[1] Setting {self.serial} to discoverable mode...")
        self.adb("shell", "am", "start", "-a", 
                 "android.bluetooth.adapter.action.REQUEST_DISCOVERABLE",
                 "--ei", "android.bluetooth.adapter.extra.DISCOVERABLE_DURATION", "120")
        time.sleep(2)
        
        # Auto-dismiss discoverable permission dialog if present
        for _ in range(5):
            focus = self.adb("shell", "dumpsys", "window", "windows")
            if "RequestPermission" in focus:
                # Find and tap "Allow" / "許可"
                self._tap_button(["許可", "Allow", "OK", "はい"])
                time.sleep(1)
                break
            time.sleep(0.5)
        print("[1] Discoverable mode set")
    
    def step2_pc_pair(self):
        """PC: trigger WinRT PairAsync in background"""
        print("[2] Starting PC-side pairing...")
        
        ps_script = f'''
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        [Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime] | Out-Null
        [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime] | Out-Null
        
        $asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | 
            Where-Object {{ $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' }})[0]
        
        Function Await($WinRtTask, $ResultType) {{
            $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
            $netTask = $asTask.Invoke($null, @($WinRtTask))
            $netTask.Wait(30000) | Out-Null
            $netTask.Result
        }}
        
        $selector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false)
        $devices = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($selector)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
        
        $macClean = "{self.bt_mac.replace(':', '').upper()}"
        $target = $null
        foreach ($d in $devices) {{
            if ($d.Id.ToUpper() -like "*$macClean*") {{
                $target = $d
                break
            }}
        }}
        
        if ($null -eq $target) {{
            Write-Host "PAIR_RESULT:NOT_FOUND"
            exit 1
        }}
        
        Write-Host "PAIR_TARGET:$($target.Name)"
        
        $btDevice = Await ([Windows.Devices.Bluetooth.BluetoothDevice]::FromIdAsync($target.Id)) ([Windows.Devices.Bluetooth.BluetoothDevice])
        $di = $btDevice.DeviceInformation
        
        if ($di.Pairing.IsPaired) {{
            Write-Host "PAIR_RESULT:ALREADY_PAIRED"
            exit 0
        }}
        
        $result = Await ($di.Pairing.PairAsync()) ([Windows.Devices.Enumeration.DevicePairingResult])
        Write-Host "PAIR_RESULT:$($result.Status)"
        '''
        
        try:
            r = subprocess.run(
                ["powershell", "-ExecutionPolicy", "Bypass", "-Command", ps_script],
                capture_output=True, text=True, timeout=45
            )
            output = r.stdout.strip()
            print(f"[2] PC output: {output}")
            
            for line in output.split('\n'):
                if "PAIR_RESULT:" in line:
                    self.pair_result = line.split("PAIR_RESULT:")[1].strip()
        except Exception as e:
            print(f"[2] PC error: {e}")
            self.pair_result = "ERROR"
    
    def step3_android_accept(self):
        """Android: monitor and auto-accept pairing dialog"""
        print("[3] Monitoring Android for pairing dialog...")
        
        for attempt in range(30):
            focus = self.adb("shell", "dumpsys", "window")
            
            # Check for Bluetooth pairing dialog
            if any(kw in focus for kw in ["BluetoothPair", "RequestPermission", "bluetooth_pin"]):
                print(f"[3] Pairing dialog detected (attempt {attempt})")
                time.sleep(0.5)
                
                # Try UI automator to find pair button
                if self._tap_button(["ペア設定する", "ペアリング", "Pair", "PAIR", "OK"]):
                    print("[3] Pair button tapped!")
                    return True
                
                # Fallback: key events
                print("[3] Fallback: sending TAB+ENTER")
                self.adb("shell", "input", "keyevent", "KEYCODE_TAB")
                time.sleep(0.3)
                self.adb("shell", "input", "keyevent", "KEYCODE_ENTER")
                return True
            
            # Also check for PIN confirmation dialog
            if "confirm" in focus.lower() and "bluetooth" in focus.lower():
                print(f"[3] PIN confirmation dialog detected")
                self._tap_button(["OK", "確認", "ペア設定する", "Pair"])
                return True
                
            time.sleep(1)
        
        print("[3] No pairing dialog appeared within 30s")
        return False
    
    def _tap_button(self, labels):
        """Find button by text label in UI dump and tap it"""
        try:
            dump = self.adb("shell", "uiautomator", "dump", "/dev/tty", timeout=5)
            if not dump:
                return False
            
            for label in labels:
                # Find bounds for this text
                pattern = rf'text="{re.escape(label)}"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"'
                m = re.search(pattern, dump)
                if m:
                    x1, y1, x2, y2 = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
                    cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
                    print(f"  Tapping '{label}' at ({cx}, {cy})")
                    self.adb("shell", "input", "tap", str(cx), str(cy))
                    return True
            
            # Try content-desc too
            for label in labels:
                pattern = rf'content-desc="{re.escape(label)}"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"'
                m = re.search(pattern, dump)
                if m:
                    x1, y1, x2, y2 = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
                    cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
                    print(f"  Tapping desc '{label}' at ({cx}, {cy})")
                    self.adb("shell", "input", "tap", str(cx), str(cy))
                    return True
                    
        except Exception as e:
            print(f"  UI dump error: {e}")
        
        return False
    
    def run(self):
        """Execute full auto-pairing sequence"""
        print(f"=== Bluetooth Auto-Pair ===")
        print(f"  Device: {self.serial}")
        print(f"  BT MAC: {self.bt_mac}")
        print()
        
        # Step 1: Make discoverable
        self.step1_discoverable()
        
        # Step 2 & 3: Run PC pairing and Android accept in parallel
        pc_thread = threading.Thread(target=self.step2_pc_pair)
        android_thread = threading.Thread(target=self.step3_android_accept)
        
        pc_thread.start()
        time.sleep(1)  # Give PC a head start
        android_thread.start()
        
        pc_thread.join(timeout=45)
        android_thread.join(timeout=35)
        
        # Result
        print()
        if self.pair_result and self.pair_result in ["Paired", "ALREADY_PAIRED", "AlreadyPaired"]:
            print(f"✅ Pairing SUCCESS: {self.pair_result}")
            return True
        else:
            print(f"❌ Pairing result: {self.pair_result}")
            return False


def main():
    # Get device info
    r = subprocess.run(["adb", "devices"], capture_output=True, text=True)
    usb_devices = []
    for line in r.stdout.strip().split('\n')[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == 'device' and ':' not in parts[0]:
            serial = parts[0]
            # Get BT MAC
            r2 = subprocess.run(["adb", "-s", serial, "shell", "settings", "get", "secure", "bluetooth_address"],
                               capture_output=True, text=True, timeout=5)
            mac = r2.stdout.strip()
            if mac and ':' in mac:
                usb_devices.append((serial, mac))
                print(f"  {serial} -> BT:{mac}")
    
    if not usb_devices:
        print("No USB devices found")
        sys.exit(1)
    
    # Pair each device
    for serial, mac in usb_devices:
        pairer = BluetoothAutoPair(serial, mac)
        success = pairer.run()
        print()
        if not success:
            print(f"  Retrying {serial} might need manual intervention")


if __name__ == "__main__":
    main()
