#!/usr/bin/env python3
"""Bluetooth Auto-Pairing - single device test"""
import subprocess, threading, time, re, sys, os

os.environ['PYTHONIOENCODING'] = 'utf-8'

class BluetoothAutoPair:
    def __init__(self, adb_serial, bt_mac):
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
        print(f"[1] Setting {self.serial} to discoverable...")
        self.adb("shell", "am", "start", "-a", 
                 "android.bluetooth.adapter.action.REQUEST_DISCOVERABLE",
                 "--ei", "android.bluetooth.adapter.extra.DISCOVERABLE_DURATION", "120")
        
        # Wait and auto-dismiss permission dialog
        for i in range(8):
            time.sleep(1)
            focus = self.adb("shell", "dumpsys", "window")
            if "RequestPermission" in focus:
                print("[1] Permission dialog found, accepting...")
                self._tap_button(["許可", "Allow", "OK"])
                time.sleep(1)
                break
        
        print("[1] Discoverable mode ready")
    
    def step2_pc_pair(self):
        print("[2] PC-side pairing starting...")
        mac_lower = self.bt_mac.lower()
        
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
        
        # Search unpaired
        $selector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false)
        $devices = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($selector)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
        
        Write-Host "SCAN_COUNT:$($devices.Count)"
        foreach ($d in $devices) {{ Write-Host "SCAN_DEV:$($d.Name)|$($d.Id)" }}
        
        $target = $null
        foreach ($d in $devices) {{
            if ($d.Id.ToLower().Contains("{mac_lower}")) {{
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
        
        if ($di.Pairing.IsPaired) {{ Write-Host "PAIR_RESULT:AlreadyPaired"; exit 0 }}
        if (-not $di.Pairing.CanPair) {{ Write-Host "PAIR_RESULT:CannotPair"; exit 1 }}
        
        $result = Await ($di.Pairing.PairAsync()) ([Windows.Devices.Enumeration.DevicePairingResult])
        Write-Host "PAIR_RESULT:$($result.Status)"
        '''
        
        try:
            r = subprocess.run(
                ["powershell", "-ExecutionPolicy", "Bypass", "-Command", ps_script],
                capture_output=True, text=True, timeout=45
            )
            for line in (r.stdout + r.stderr).split('\n'):
                line = line.strip()
                if line:
                    print(f"  [PC] {line}")
                if "PAIR_RESULT:" in line:
                    self.pair_result = line.split("PAIR_RESULT:")[1].strip()
        except Exception as e:
            print(f"  [PC] Error: {e}")
            self.pair_result = "ERROR"
    
    def step3_android_accept(self):
        print("[3] Monitoring Android for pairing dialog...")
        
        for attempt in range(30):
            focus = self.adb("shell", "dumpsys", "window")
            
            if any(kw in focus for kw in ["BluetoothPair", "bluetooth_pin", 
                                           "com.android.settings.bluetooth"]):
                print(f"[3] Dialog detected (attempt {attempt})")
                time.sleep(0.5)
                
                if self._tap_button(["ペア設定する", "ペアリング", "Pair", "PAIR", "OK"]):
                    print("[3] Button tapped!")
                    return True
                
                # Fallback
                self.adb("shell", "input", "keyevent", "KEYCODE_TAB")
                time.sleep(0.3)
                self.adb("shell", "input", "keyevent", "KEYCODE_ENTER")
                return True
            
            time.sleep(1)
        
        print("[3] No pairing dialog within 30s")
        return False
    
    def _tap_button(self, labels):
        try:
            dump = self.adb("shell", "uiautomator", "dump", "/dev/tty", timeout=5)
            if not dump:
                return False
            for label in labels:
                pattern = rf'text="{re.escape(label)}"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"'
                m = re.search(pattern, dump)
                if m:
                    cx = (int(m.group(1)) + int(m.group(3))) // 2
                    cy = (int(m.group(2)) + int(m.group(4))) // 2
                    print(f"  Tap '{label}' ({cx},{cy})")
                    self.adb("shell", "input", "tap", str(cx), str(cy))
                    return True
        except:
            pass
        return False
    
    def run(self):
        print(f"=== BT Auto-Pair: {self.serial} (MAC: {self.bt_mac}) ===")
        
        self.step1_discoverable()
        time.sleep(2)  # Extra wait for discoverable to propagate
        
        pc_thread = threading.Thread(target=self.step2_pc_pair)
        android_thread = threading.Thread(target=self.step3_android_accept)
        
        pc_thread.start()
        time.sleep(2)
        android_thread.start()
        
        pc_thread.join(timeout=45)
        android_thread.join(timeout=35)
        
        print()
        if self.pair_result in ["Paired", "AlreadyPaired"]:
            print(f"[OK] SUCCESS: {self.pair_result}")
            return True
        else:
            print(f"[NG] Result: {self.pair_result}")
            return False

if __name__ == "__main__":
    serial = sys.argv[1] if len(sys.argv) > 1 else "A9250700956"
    
    r = subprocess.run(["adb", "-s", serial, "shell", "settings", "get", "secure", "bluetooth_address"],
                       capture_output=True, text=True, timeout=5)
    mac = r.stdout.strip()
    
    if not mac or ':' not in mac:
        print(f"Failed to get BT MAC for {serial}")
        sys.exit(1)
    
    print(f"Device: {serial}, BT MAC: {mac}")
    pairer = BluetoothAutoPair(serial, mac)
    success = pairer.run()
    sys.exit(0 if success else 1)
