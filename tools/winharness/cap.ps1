param([string]$Out, [int]$W = 1600, [int]$H = 1000)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
if (-not ("CapW32" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public class CapW32 {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
"@
}
$p = Get-Process qcview -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "app gone"; exit 1 }
$h = $p.MainWindowHandle
[CapW32]::SetWindowPos($h, [IntPtr]::Zero, 40, 40, $W, $H, 0x0040) | Out-Null
Start-Sleep -Milliseconds 800
$r = New-Object CapW32+RECT
[CapW32]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($Out)
"$w x $hh -> $Out"
