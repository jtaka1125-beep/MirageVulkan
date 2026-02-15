Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;

public class Win32 {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    public const int SW_RESTORE = 9;
    public const int SW_SHOW = 5;
}
"@

$found = $false
$targetHwnd = [IntPtr]::Zero
$allMatches = @()

[Win32]::EnumWindows({
    param($hwnd, $lParam)
    if ([Win32]::IsWindowVisible($hwnd)) {
        $sb = New-Object System.Text.StringBuilder 256
        [Win32]::GetWindowText($hwnd, $sb, 256) | Out-Null
        $title = $sb.ToString()
        if ($title -match 'Mirage') {
            Write-Host "Found window: '$title' (hwnd: $hwnd)"
            $script:targetHwnd = $hwnd
            $script:found = $true
        }
    }
    return $true
}, [IntPtr]::Zero) | Out-Null

if ($found) {
    Write-Host ""
    Write-Host "Bringing window to foreground: hwnd = $targetHwnd"
    [Win32]::ShowWindow($targetHwnd, [Win32]::SW_RESTORE) | Out-Null
    [Win32]::SetForegroundWindow($targetHwnd) | Out-Null
    Write-Host "SetForegroundWindow called successfully."
    Start-Sleep -Seconds 2
    Write-Host "Done. Window should now be in foreground."
} else {
    Write-Host "No window with 'Mirage' in the title was found."
}
