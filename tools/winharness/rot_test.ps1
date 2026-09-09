param([string]$Phase)
Add-Type -AssemblyName System.Windows.Forms
$sp  = $(if ($env:QCV_HARNESS_DIR) { $env:QCV_HARNESS_DIR } else { $PSScriptRoot })
$exe = "C:\Users\uniongraphics\Documents\GitHub\QCView-Player\build-release\qcview.exe"
$wd  = Split-Path $exe
function Cap($name) { & "$sp\cap.ps1" -Out "$sp\rot_$name.png" | Out-Null; "$(Get-Date -Format HH:mm:ss.fff) cap $name" }
function Fwd($path) { Start-Process $exe -ArgumentList "`"$path`"" -WorkingDirectory $wd | Out-Null; "$(Get-Date -Format HH:mm:ss.fff) fwd $path" }
function Space() { $p = Get-Process qcview | Select-Object -First 1; (New-Object -ComObject WScript.Shell).AppActivate($p.Id) | Out-Null; Start-Sleep -Milliseconds 300; [System.Windows.Forms.SendKeys]::SendWait(" "); "$(Get-Date -Format HH:mm:ss.fff) SPACE" }
if ($Phase -eq "playlist") {
  $p = Start-Process $exe -ArgumentList "--playlist-test", "`"$sp\rotpl`"" -WorkingDirectory $wd -PassThru
  Start-Sleep 8; Space; Start-Sleep 2; Cap "pl_a"; Start-Sleep 4; Cap "pl_b"; Start-Sleep 4; Cap "pl_c"
  Start-Sleep 2; $p.CloseMainWindow() | Out-Null; Start-Sleep 3
}
if ($Phase -eq "fwd") {
  $p = Start-Process $exe -ArgumentList "--playlist-test", "`"$sp\rotpl`"" -WorkingDirectory $wd -PassThru
  Start-Sleep 8; Space; Start-Sleep 6; Cap "fwd_onb"; Fwd "$sp\rotpl\a_normal.mov"; Start-Sleep 5; Cap "fwd_a"
  $p.CloseMainWindow() | Out-Null; Start-Sleep 3
}
if ($Phase -eq "single") {
  $p = Start-Process $exe -ArgumentList "`"$sp\rotpl\b_rot180.mov`"" -WorkingDirectory $wd -PassThru
  Start-Sleep 7; Cap "single_b"; Fwd "$sp\rotpl_bad.mov"; Start-Sleep 4; Cap "single_bad"; Fwd "$sp\rotpl\a_normal.mov"; Start-Sleep 5; Cap "single_a"
  $p.CloseMainWindow() | Out-Null; Start-Sleep 3
}
"done $Phase alive=$((Get-Process qcview -ErrorAction SilentlyContinue) -ne $null)"
if ($Phase -eq "fwd2") {
  if (-not ("ClickW32" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public class ClickW32 {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
"@
  }
  function Click($dx, $dy) {
    $q = Get-Process qcview | Select-Object -First 1
    $r = New-Object ClickW32+RECT
    [ClickW32]::GetWindowRect($q.MainWindowHandle, [ref]$r) | Out-Null
    [ClickW32]::SetCursorPos($r.L + $dx, $r.T + $dy) | Out-Null
    Start-Sleep -Milliseconds 150
    [ClickW32]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    [ClickW32]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 90; [ClickW32]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [ClickW32]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    "$(Get-Date -Format HH:mm:ss.fff) click $dx,$dy"
  }
  $p = Start-Process $exe -ArgumentList "--playlist-test", "`"$sp\rotpl`"" -WorkingDirectory $wd -PassThru
  Start-Sleep 8; Space; Start-Sleep 6; Cap "fwd2_onb"
  Click 120 222; Start-Sleep 5; Cap "fwd2_a"
  Click 120 252; Start-Sleep 5; Cap "fwd2_b"
  Click 120 222; Start-Sleep 5; Cap "fwd2_a2"
  $p.CloseMainWindow() | Out-Null; Start-Sleep 3
  "done fwd2 alive=$((Get-Process qcview -ErrorAction SilentlyContinue) -ne $null)"
}
