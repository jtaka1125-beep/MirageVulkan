# Bluetooth auto-pair v3 - C# inline for Custom Pairing with auto-accept
param(
    [string]$TargetMAC = ""
)

Write-Host "=== BT Auto-Pair v3 (Custom Pairing) ===" -ForegroundColor Cyan

# Embed C# class for WinRT Bluetooth pairing
$csharp = @"
using System;
using System.Threading;
using System.Threading.Tasks;
using Windows.Devices.Bluetooth;
using Windows.Devices.Enumeration;

public class BtPairer
{
    public static string PairDevice(string targetMacLower)
    {
        try
        {
            var selector = BluetoothDevice.GetDeviceSelectorFromPairingState(false);
            var devices = DeviceInformation.FindAllAsync(selector).AsTask().GetAwaiter().GetResult();
            
            Console.WriteLine("SCAN_COUNT:" + devices.Count);
            foreach (var d in devices)
                Console.WriteLine("SCAN_DEV:" + d.Name + "|" + d.Id);
            
            DeviceInformation target = null;
            foreach (var d in devices)
            {
                if (d.Id.ToLower().Contains(targetMacLower))
                {
                    target = d;
                    break;
                }
            }
            
            if (target == null) return "NOT_FOUND";
            
            Console.WriteLine("PAIR_TARGET:" + target.Name);
            
            var btDevice = BluetoothDevice.FromIdAsync(target.Id).AsTask().GetAwaiter().GetResult();
            var di = btDevice.DeviceInformation;
            
            if (di.Pairing.IsPaired) return "AlreadyPaired";
            if (!di.Pairing.CanPair) return "CannotPair";
            
            // Custom pairing with auto-accept
            var customPairing = di.Pairing.Custom;
            customPairing.PairingRequested += (sender, args) =>
            {
                Console.WriteLine("PAIRING_KIND:" + args.PairingKind);
                switch (args.PairingKind)
                {
                    case DevicePairingKinds.ConfirmOnly:
                        Console.WriteLine("AUTO_ACCEPT:ConfirmOnly");
                        args.Accept();
                        break;
                    case DevicePairingKinds.ConfirmPinMatch:
                        Console.WriteLine("AUTO_ACCEPT:ConfirmPinMatch PIN=" + args.Pin);
                        args.Accept();
                        break;
                    case DevicePairingKinds.DisplayPin:
                        Console.WriteLine("DISPLAY_PIN:" + args.Pin);
                        args.Accept();
                        break;
                    case DevicePairingKinds.ProvidePin:
                        Console.WriteLine("PROVIDE_PIN:0000");
                        args.Accept("0000");
                        break;
                    default:
                        Console.WriteLine("UNKNOWN_KIND:" + args.PairingKind);
                        args.Accept();
                        break;
                }
            };
            
            var kinds = DevicePairingKinds.ConfirmOnly 
                      | DevicePairingKinds.ConfirmPinMatch 
                      | DevicePairingKinds.DisplayPin 
                      | DevicePairingKinds.ProvidePin;
            
            var result = customPairing.PairAsync(kinds).AsTask().GetAwaiter().GetResult();
            return result.Status.ToString();
        }
        catch (Exception ex)
        {
            return "ERROR:" + ex.Message;
        }
    }
}
"@

# Compile C# with WinRT references
try {
    Add-Type -TypeDefinition $csharp -ReferencedAssemblies @(
        "System.Runtime.WindowsRuntime",
        "$([System.Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory())WinMetadata\Windows.Devices.winmd",
        "$([System.Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory())WinMetadata\Windows.Foundation.winmd"
    ) -Language CSharp
    Write-Host "[OK] C# compiled" -ForegroundColor Green
} catch {
    Write-Host "[ERROR] C# compilation failed: $_" -ForegroundColor Red
    exit 1
}

# Run pairing
$macLower = $TargetMAC.ToLower()
Write-Host "Target MAC: $macLower" -ForegroundColor Yellow
Write-Host ""

$result = [BtPairer]::PairDevice($macLower)
Write-Host ""
Write-Host "PAIR_RESULT:$result" -ForegroundColor $(if ($result -eq "Paired" -or $result -eq "AlreadyPaired") { "Green" } else { "Red" })

exit $(if ($result -eq "Paired" -or $result -eq "AlreadyPaired") { 0 } else { 1 })
