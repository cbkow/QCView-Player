param([string]$Mode)
$sp  = "C:\Users\UNIONG~1\AppData\Local\Temp\claude\C--Users-uniongraphics-Documents-GitHub-QCView-Player\227456b9-cda2-4db4-bcc4-7c0861e2d745\scratchpad"
$exe = "C:\Users\uniongraphics\Documents\GitHub\QCView-Player\build-release\qcview.exe"
$clips = @(
  "$sp\fmt9\vvc_420p10_720p.mp4",
  "$sp\fmt9\ffv1_422p10.mkv",
  "$sp\fmt9\ffv1_420p8.mov",
  "$sp\fmt9\ffv1_gbrp12.mkv",
  "C:\Volumes\union-ny-gfx\union-jobs\000000_SYNC\CW  .  BALANCING MACRO AND MICRO CONCERNS.mov",
  "C:\Volumes\union-ny-gfx\union-jobs\000000_SYNC\01_A001_09211049_C068.mov"
)
$env:QCV_DUMP_FRAME = "$sp\parity\$Mode"
if ($Mode -eq "legacy") { $env:QCV_SWS_THREADS = "1"; $env:QCV_SWS_LEGACY = "1" } else { $env:QCV_SWS_LEGACY = $null; $env:QCV_SWS_THREADS = $null }
$env:HKCU_HW = ""
$p = Start-Process $exe -ArgumentList "`"$($clips[0])`"" -WorkingDirectory (Split-Path $exe) -PassThru
Start-Sleep 6
foreach ($c in $clips[1..($clips.Count-1)]) { & $exe "$c"; Start-Sleep 6 }
Start-Sleep 2
$p.CloseMainWindow() | Out-Null; Start-Sleep 3
Get-Content "C:\Users\uniongraphics\Documents\GitHub\QCView-Player\build-release\qcview-log.txt" | Select-String "dumping first" | ForEach-Object { $_.Line.Substring(24, [Math]::Min(120, $_.Line.Length - 24)) }
"done $Mode alive=$((Get-Process qcview -ErrorAction SilentlyContinue) -ne $null)"
