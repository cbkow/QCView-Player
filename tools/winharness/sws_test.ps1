param([string]$Threads = "")
Add-Type -AssemblyName System.Windows.Forms
$sp  = $(if ($env:QCV_HARNESS_DIR) { $env:QCV_HARNESS_DIR } else { $PSScriptRoot })
$exe = "C:\Users\uniongraphics\Documents\GitHub\QCView-Player\build-release\qcview.exe"
$log = "C:\Users\uniongraphics\Documents\GitHub\QCView-Player\build-release\qcview-log.txt"
$raw = "C:\Volumes\union-ny-gfx\union-jobs\000000_SYNC\01_A001_09211049_C068.mov"
$dnx = "C:\Volumes\union-ny-gfx\union-jobs\000000_SYNC\CW  .  BALANCING MACRO AND MICRO CONCERNS.mov"
if ($Threads -ne "") { $env:QCV_SWS_THREADS = $Threads } else { $env:QCV_SWS_THREADS = $null }
$sig = '[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();'
if (-not ("X.FG3" -as [type])) { Add-Type -MemberDefinition $sig -Name FG3 -Namespace X | Out-Null }
$p = Start-Process $exe -ArgumentList "`"$raw`"" -WorkingDirectory (Split-Path $exe) -PassThru
Start-Sleep 7
if ([X.FG3]::GetForegroundWindow() -ne $p.MainWindowHandle) { "app not foreground - abort"; $p.CloseMainWindow() | Out-Null; exit }
[System.Windows.Forms.SendKeys]::SendWait(" "); Start-Sleep 10
$c1 = (Get-Process -Id $p.Id).CPU; Start-Sleep 3; $c2 = (Get-Process -Id $p.Id).CPU
"threads=$Threads cpu-seconds/3s: $([math]::Round($c2 - $c1, 1))"
[System.Windows.Forms.SendKeys]::SendWait(" "); Start-Sleep 1
& "$sp\cap.ps1" -Out "$sp\sws_raw_$Threads.png" | Out-Null
Get-Content $log | Select-String -Pattern "swscale .* ms/frame|AudioPlayer: drift" | Select-Object -Last 4 | ForEach-Object { "  " + $_.Line.Substring(24, [Math]::Min(110, $_.Line.Length - 24)) }
if ($Threads -eq "") {
  & $exe "$sp\fmt9\ffv1_422p10.mkv"; Start-Sleep 5; & "$sp\cap.ps1" -Out "$sp\sws_ffv1.png" | Out-Null
  & $exe "$dnx"; Start-Sleep 6; & "$sp\cap.ps1" -Out "$sp\sws_dnx.png" | Out-Null
}
$p.CloseMainWindow() | Out-Null; Start-Sleep 3
"done alive=$((Get-Process qcview -ErrorAction SilentlyContinue) -ne $null)"
