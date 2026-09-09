param([string]$Exe, [string]$Label, [string]$Dir = "")
Set-Location "C:\Users\uniongraphics\Documents\GitHub\QCView-Player"
$logPath = Join-Path (Split-Path $Exe -Parent) "qcview-log.txt"
if (Get-Process qcview -ErrorAction SilentlyContinue) { "ALREADY RUNNING"; exit 1 }
Add-Type -AssemblyName System.Windows.Forms
if (-not ("KBH" -as [type])) { Add-Type @"
using System; using System.Runtime.InteropServices;
public static class KBH {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static void Hold(byte vk, int ms){ keybd_event(vk,0,0,UIntPtr.Zero); System.Threading.Thread.Sleep(ms); keybd_event(vk,0,2,UIntPtr.Zero); }
  public static uint FgPid(){ uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
}
"@ }
function Alive { return [bool](Get-Process qcview -ErrorAction SilentlyContinue) }
function Stamp($m) { "$(Get-Date -Format HH:mm:ss.fff) [$Label] $m" }
function Wait-Alive($sec) { for ($i=0; $i -lt $sec; $i++) { Start-Sleep 1; if (-not (Alive)) { Stamp "PROCESS GONE at wait second $i"; return $false } }; return $true }
$dir = $Dir
$p = Start-Process -FilePath $Exe -ArgumentList "--playlist-test", "`"$dir`"" -WorkingDirectory (Split-Path $Exe -Parent) -PassThru
Stamp "launched pid $($p.Id)"
if (-not (Wait-Alive 14)) { exit 1 }
(New-Object -ComObject WScript.Shell).AppActivate($p.Id) | Out-Null; Start-Sleep -Milliseconds 700
if ([KBH]::FgPid() -ne $p.Id) { Stamp "qcview NOT foreground - abort"; exit 1 }
[System.Windows.Forms.SendKeys]::SendWait(" "); Stamp "SPACE"
if (-not (Wait-Alive 30)) { exit 1 }
Stamp "hold L 9s"; [KBH]::Hold(0x4C, 9000); if (-not (Wait-Alive 2)) { exit 1 }
Stamp "hold J 7s"; [KBH]::Hold(0x4A, 7000); if (-not (Wait-Alive 2)) { exit 1 }
[System.Windows.Forms.SendKeys]::SendWait("{END}"); if (-not (Wait-Alive 2)) { exit 1 }
[System.Windows.Forms.SendKeys]::SendWait("{HOME}"); if (-not (Wait-Alive 2)) { exit 1 }
Stamp "hold L 6s"; [KBH]::Hold(0x4C, 6000); if (-not (Wait-Alive 2)) { exit 1 }
Stamp "hold J 5s"; [KBH]::Hold(0x4A, 5000); if (-not (Wait-Alive 2)) { exit 1 }
[System.Windows.Forms.SendKeys]::SendWait(" "); if (-not (Wait-Alive 2)) { exit 1 }
Stamp "alive after hammer"
$log = Get-Content $logPath
$gf = $log | Select-String 'get_format codec=prores'
$dbl = 0; for ($i=1; $i -lt $gf.Count; $i++) { $a=$gf[$i-1].Line; $b=$gf[$i].Line; if (($a -match 'ctx=(0x[0-9a-f]+)') ) { $ca=$Matches[1]; if ($b -match 'ctx=(0x[0-9a-f]+)' -and $Matches[1] -eq $ca) { $dbl++ } } }
"[$Label] get_format=$($gf.Count) repeat-on-same-ctx=$dbl newPool=$(($log | Select-String 'new frame pool').Count) cacheHIT=$(($log | Select-String 'output cache HIT').Count) alloc=$(($log | Select-String 'RGBA16F allocated').Count) DEVICE_LOST=$(($log | Select-String 'DEVICE_LOST').Count) submitFail=$(($log | Select-String 'vkQueueSubmit').Count) nvEvents=$((Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'; StartTime=$p.StartTime} -ErrorAction SilentlyContinue).Count)"
$gf | Select-Object -First 1 | ForEach-Object { $_.Line -replace '.*\[ctx','  first get_format: [ctx' }
$pp = Get-Process qcview -ErrorAction SilentlyContinue; if ($pp) { $pp.CloseMainWindow() | Out-Null; Start-Sleep 3 }

