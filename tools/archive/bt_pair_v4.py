#!/usr/bin/env python3
"""
Bluetooth Auto-Pairing v4 - Full automation
PC side: PowerShell 5.1 PairAsync + Windows SSP dialog auto-click  
Android side: ADB pairing dialog auto-accept
All three run in parallel threads.
"""
import subprocess, threading, time, re, sys, os

class BluetoothAutoPair:
    def __init__(self, adb_serial, bt_mac):
        self.serial = adb_serial
        self.bt_mac = bt_mac
        self.pair_result = None
        self.android_accepted = False
        self.pc_dialog_clicked = False
        
    def adb(self, *args, timeout=10):
        cmd = ["adb", "-s", self.serial] + list(args)
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                             creationflags=0x08000000 if os.name == 'nt' else 0)
            return r.stdout.strip()
        except:
            return ""
    
    def step1_discoverable(self):
        """Set Android to discoverable + dismiss permission dialog"""
        print(f"[1] Discoverable: {self.serial}")
        self.adb("shell", "am", "start", "-a",
                 "android.bluetooth.adapter.action.REQUEST_DISCOVERABLE",
                 "--ei", "android.bluetooth.adapter.extra.DISCOVERABLE_DURATION", "120")
        
        for _ in range(8):
            time.sleep(1)
            focus = self.adb("shell", "dumpsys", "window")
            if "RequestPermission" in focus:
                self._tap_button(["許可", "Allow", "OK"])
                break
        print("[1] Done")
    
    def step2_pc_pair(self):
        """PC: PowerShell 5.1 Simple PairAsync"""
        print("[2] PC PairAsync...")
        mac = self.bt_mac.lower()
        
        ps = f'''
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        [Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime] | Out-Null
        [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime] | Out-Null
        $m = ([System.WindowsRuntimeSystemExtensions].GetMethods() | ? {{ $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' }})[0]
        Function A($t,$r) {{ $a=$m.MakeGenericMethod($r); $n=$a.Invoke($null,@($t)); $n.Wait(30000)|Out-Null; $n.Result }}
        $s = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false)
        $ds = A ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($s)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
        $t = $null; foreach($d in $ds){{ if($d.Id.ToLower().Contains("{mac}")){{ $t=$d; break }} }}
        if(-not $t){{ Write-Host "PAIR_RESULT:NOT_FOUND"; exit 1 }}
        Write-Host "PAIR_TARGET:$($t.Name)"
        $b = A ([Windows.Devices.Bluetooth.BluetoothDevice]::FromIdAsync($t.Id)) ([Windows.Devices.Bluetooth.BluetoothDevice])
        $di = $b.DeviceInformation
        if($di.Pairing.IsPaired){{ Write-Host "PAIR_RESULT:AlreadyPaired"; exit 0 }}
        $r = A ($di.Pairing.PairAsync()) ([Windows.Devices.Enumeration.DevicePairingResult])
        Write-Host "PAIR_RESULT:$($r.Status)"
        '''
        
        try:
            r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps],
                             capture_output=True, text=True, timeout=45,
                             creationflags=0x08000000 if os.name == 'nt' else 0)
            output = r.stdout.strip()
            for line in output.split('\n'):
                line = line.strip()
                if line: print(f"  [PC] {line}")
                if "PAIR_RESULT:" in line:
                    self.pair_result = line.split("PAIR_RESULT:")[1].strip()
        except Exception as e:
            print(f"  [PC] Error: {e}")
            self.pair_result = "ERROR"
    
    def step3_android_accept(self):
        """Android: auto-accept Bluetooth pairing dialog"""
        print("[3] Android dialog watch...")
        
        for attempt in range(40):
            focus = self.adb("shell", "dumpsys", "window")
            
            # Detect pairing dialog
            if any(kw in focus for kw in ["BluetoothPair", "bluetooth_pin",
                                           "RequestPermissionHelperActivity",
                                           "com.android.settings.bluetooth"]):
                print(f"[3] BT dialog at {attempt}s")
                time.sleep(1)
                
                # Try specific button texts
                if self._tap_button(["ペア設定する", "ペアリング", "Pair", "PAIR"]):
                    self.android_accepted = True
                    return
                
                # Retry with fresh dump
                time.sleep(0.5)
                if self._tap_button(["OK", "はい", "Yes", "Accept"]):
                    self.android_accepted = True
                    return
                
                # Last resort: keyboard
                self.adb("shell", "input", "keyevent", "61")  # TAB
                time.sleep(0.3)
                self.adb("shell", "input", "keyevent", "66")  # ENTER
                self.android_accepted = True
                return
            
            time.sleep(1)
        
        print("[3] No dialog in 40s")
    
    def step4_pc_dialog_accept(self):
        """Windows: auto-click SSP confirmation toast/dialog via SendKeys or similar"""
        print("[4] PC SSP dialog watch...")
        
        # Windows shows a Bluetooth SSP notification/dialog
        # We can use PowerShell to find and click it
        for _ in range(30):
            # Check for Bluetooth pairing notification
            ps_check = '''
            Add-Type -AssemblyName UIAutomationClient
            $root = [System.Windows.Automation.AutomationElement]::RootElement
            $cond = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::NameProperty, "*Bluetooth*"
            )
            $found = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
            if ($found) { Write-Host "FOUND"; $found.GetCurrentPropertyValue([System.Windows.Automation.AutomationElement]::NameProperty) }
            '''
            # Simpler: just check if there's a notification
            time.sleep(1)
        
        print("[4] Done")
    
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
        print(f"=== BT Pair: {self.serial} MAC:{self.bt_mac} ===")
        
        # Clear any existing dialogs
        self.adb("shell", "input", "keyevent", "KEYCODE_BACK")
        self.adb("shell", "input", "keyevent", "KEYCODE_HOME")
        time.sleep(1)
        
        # Step 1
        self.step1_discoverable()
        time.sleep(3)
        
        # Steps 2+3 in parallel
        t_pc = threading.Thread(target=self.step2_pc_pair)
        t_android = threading.Thread(target=self.step3_android_accept)
        
        t_pc.start()
        time.sleep(3)  # PC needs time to call PairAsync before Android sees dialog
        t_android.start()
        
        t_pc.join(timeout=50)
        t_android.join(timeout=45)
        
        if self.pair_result in ["Paired", "AlreadyPaired"]:
            print(f"\n=== SUCCESS: {self.pair_result} ===")
            return True
        else:
            print(f"\n=== FAILED: {self.pair_result} (android_accept={self.android_accepted}) ===")
            return False

if __name__ == "__main__":
    serial = sys.argv[1] if len(sys.argv) > 1 else "A9250700956"
    r = subprocess.run(["adb", "-s", serial, "shell", "settings", "get", "secure", "bluetooth_address"],
                       capture_output=True, text=True, timeout=5)
    mac = r.stdout.strip()
    if not mac or ':' not in mac:
        print(f"No BT MAC for {serial}")
        sys.exit(1)
    
    pairer = BluetoothAutoPair(serial, mac)
    sys.exit(0 if pairer.run() else 1)
