"""
AOA Switch for Development Environment
ADBドライバ占有を一時回避してAOAモード切替を行う

フロー:
1. ADBデバイスを一時的にdisable
2. libusb-1.0でデバイスをopen (WinUSBバックエンド)
3. AOAベンダーコマンド送信
4. デバイスがAOAモードで再列挙 (VID=18D1)
"""
import subprocess
import sys
import time
import os

def run(cmd, check=True):
    """Run command and return output"""
    print(f"  > {cmd}")
    r = subprocess.run(cmd, capture_output=True, text=True, shell=True)
    if r.stdout.strip():
        print(f"    {r.stdout.strip()}")
    if r.stderr.strip():
        print(f"    [stderr] {r.stderr.strip()}")
    if check and r.returncode != 0:
        print(f"    [exit: {r.returncode}]")
    return r

def run_admin(cmd):
    """Run command as administrator"""
    print(f"  > [ADMIN] {cmd}")
    ps_cmd = f'Start-Process -FilePath "cmd.exe" -ArgumentList "/c {cmd}" -Verb RunAs -Wait'
    return subprocess.run(["powershell", "-Command", ps_cmd], 
                         capture_output=True, text=True)

def get_adb_device_instances():
    """Get MediaTek ADB device instance IDs"""
    r = run('pnputil /enum-devices /class "AndroidUsbDeviceClass"', check=False)
    instances = []
    lines = r.stdout.split('\n')
    for i, line in enumerate(lines):
        if 'Instance ID:' in line and 'VID_0E8D' in line and 'PID_201C' in line:
            inst = line.split('Instance ID:')[1].strip()
            # Check if Started
            for j in range(i, min(i+6, len(lines))):
                if 'Status:' in lines[j] and 'Started' in lines[j]:
                    instances.append(inst)
                    break
    return instances

def main():
    print("=== AOA Switch (Dev Environment) ===\n")
    
    # Step 1: Find ADB device instances
    print("[1] Finding ADB devices...")
    instances = get_adb_device_instances()
    if not instances:
        print("  No active MediaTek ADB devices found")
        return 1
    
    print(f"  Found {len(instances)} device(s):")
    for inst in instances:
        print(f"    {inst}")
    
    # Step 2: Select target (first device or specified)
    target = instances[0]
    if len(sys.argv) > 1:
        serial = sys.argv[1]
        for inst in instances:
            if serial in inst:
                target = inst
                break
    
    print(f"\n[2] Target: {target}")
    
    # Step 3: Use aoa_switch.exe directly - it should work after device restart
    # Actually, let's try a different approach: use devcon to temporarily
    # remove the ADB driver, then use libusb
    
    print(f"\n[3] Disabling ADB interface temporarily...")
    # Disable the device (requires admin)
    run_admin(f'pnputil /disable-device "{target}"')
    time.sleep(2)
    
    print(f"\n[4] Running AOA switch...")
    aoa_exe = os.path.join(os.path.dirname(os.path.dirname(__file__)), 
                           'build', 'aoa_switch.exe')
    r = run(f'"{aoa_exe}"', check=False)
    
    if 'Switched to AOA: 0' in r.stdout or 'LIBUSB_ERROR' in r.stdout:
        print("\n  AOA switch via libusb-1.0 failed, trying re-enable...")
        # Re-enable the ADB device
        run_admin(f'pnputil /enable-device "{target}"')
        time.sleep(2)
        
        # Verify ADB is back
        r2 = run('adb devices', check=False)
        print("\n  ADB status after re-enable:")
        print(f"    {r2.stdout.strip()}")
        return 1
    
    print("\n[5] Waiting for AOA re-enumeration...")
    time.sleep(3)
    
    # Check for AOA device
    r = run('pnputil /enum-devices /ids "*VID_18D1*"', check=False)
    if 'VID_18D1' in r.stdout:
        print("\n  AOA device detected!")
    else:
        print("\n  No AOA device found after switch")
    
    # ADB should still work on the AOA-switched device won't have ADB
    # but other devices should be fine
    run('adb devices', check=False)
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
