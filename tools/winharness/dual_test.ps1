param([string]$A, [string]$B, [string]$Label = "dual")
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$sp  = "C:\Users\UNIONG~1\AppData\Local\Temp\claude\C--Users-uniongraphics-Documents-GitHub-QCView-Player\227456b9-cda2-4db4-bcc4-7c0861e2d745\scratchpad"
$exe = "C:\Users\uniongraphics\Documents\GitHub\QCView-Player\build-release\qcview.exe"
$log = "C:\Users\uniongraphics\Documents\GitHub\QCView-Player\build-release\qcview-log.txt"
$sig = '[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();'
if (-not ("X.FG4" -as [type])) { Add-Type -MemberDefinition $sig -Name FG4 -Namespace X | Out-Null }
$env:QCV_DUMP_FRAME = $null; $env:QCV_SWS_LEGACY = $null; $env:QCV_SWS_THREADS = $null
$p = Start-Process $exe -ArgumentList "--simulate-user", "`"$A`"", "`"$B`"" -WorkingDirectory (Split-Path $exe) -PassThru
Start-Sleep 9
if ([X.FG4]::GetForegroundWindow() -ne $p.MainWindowHandle) { "app not foreground - abort"; $p.CloseMainWindow() | Out-Null; exit }
[System.Windows.Forms.SendKeys]::SendWait(" ")
Start-Sleep 8
& "$sp\cap.ps1" -Out "$sp\${Label}_t1.png" | Out-Null
$c1 = (Get-Process -Id $p.Id).CPU
Start-Sleep 3
& "$sp\cap.ps1" -Out "$sp\${Label}_t2.png" | Out-Null
$c2 = (Get-Process -Id $p.Id).CPU
"cpu-seconds/3s: $([math]::Round($c2 - $c1, 1))"
[System.Windows.Forms.SendKeys]::SendWait(" ")
Start-Sleep 1
# how different is the B half between the two captures?
$a = New-Object System.Drawing.Bitmap "$sp\${Label}_t1.png"; $b = New-Object System.Drawing.Bitmap "$sp\${Label}_t2.png"
$diffA = 0; $diffB = 0; $n = 0
for ($y = 340; $y -lt 940; $y += 6) { for ($x = 500; $x -lt 1040; $x += 6) { $ca = $a.GetPixel($x, $y); $cb = $b.GetPixel($x, $y); $diffA += [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B); $ca = $a.GetPixel($x + 540, $y); $cb = $b.GetPixel($x + 540, $y); $diffB += [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B); $n++ } }
"mean pixel change over 3 s - left half: $([math]::Round($diffA / $n, 2))  right half: $([math]::Round($diffB / $n, 2))"
$a.Dispose(); $b.Dispose()
$p.CloseMainWindow() | Out-Null; Start-Sleep 3
Get-Content $log | Select-String -Pattern "DualVideoDecoder: (threading|chase|opened|software|vulkan|d3d11)|getBufferedFrame MISS|DualPlaybackController: opened" | Select-Object -First 14 | ForEach-Object { "  " + $_.Line.Substring(24, [Math]::Min(125, $_.Line.Length - 24)) }
"done alive=$((Get-Process qcview -ErrorAction SilentlyContinue) -ne $null)"
