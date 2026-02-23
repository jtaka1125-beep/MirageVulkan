Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Win32 {
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd,int X,int Y,int nWidth,int nHeight,bool bRepaint);
}
"@

$procs = Get-Process mirage_vulkan_debug -ErrorAction SilentlyContinue
foreach($p in $procs){
  $h = [IntPtr]$p.MainWindowHandle
  if($h -ne [IntPtr]::Zero){
    [Win32]::ShowWindowAsync($h, 9) | Out-Null  # SW_RESTORE
    [Win32]::MoveWindow($h, 100, 100, 1400, 900, $true) | Out-Null
    [Win32]::SetForegroundWindow($h) | Out-Null
    Write-Host "restored pid=$($p.Id) hwnd=$($p.MainWindowHandle)"
  }
}
