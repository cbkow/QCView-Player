# Windows test harness (2026-09)

PowerShell 5.1 scripts used to drive and verify `build-release\qcview.exe`
during the FFmpeg 9.0 / hardware-decode work. They send keys and clicks
to the running app, so the app must be in the FOREGROUND and nobody may
be using the box; every script aborts or misfires otherwise.

`$sp` (media + capture directory) defaults to this folder; point
`QCV_HARNESS_DIR` at a scratch folder with the test media instead.

| Script | What it does |
|---|---|
| `cap.ps1 -Out f.png [-W 1600 -H 1000]` | SetWindowPos the app onto the primary display and capture it |
| `hammer_dir.ps1 -Exe … -Dir <folder> -Label x` | `--playlist-test <folder>`: play, hold L/J (shuttle), Home/End; prints Vulkan pool / device-loss counters from the log |
| `hammer.ps1` | same on a fixed playlist |
| `dual_test.ps1 -A a -B b -Label x` | `--simulate-user A B` (dual view), play, two captures 3 s apart; compare the B half with `ffmpeg -lavfi psnr` on a crop |
| `rot_test.ps1 -Phase playlist|fwd2|single` | display-rotation regression (180°-tagged clip: `ffmpeg -display_rotation 180 -i in -c copy out`) |
| `parity.ps1 -Mode legacy|dynamic` | swscale parity: `QCV_SWS_LEGACY=1` vs dynamic, `QCV_DUMP_FRAME=<dir>` dumps first frames → md5 / `ffmpeg -lavfi psnr` |
| `sws_test.ps1 [-Threads n]` | RAW playback: per-100-frame conversion timing from the log, audio drift, captures |
| `srt_sender*.ps1` | looping fake SRT listener (H.264 720p / HEVC Main10 1080p); the app connects to `srt://127.0.0.1:9000` |

Test media used: `ffmpeg -f lavfi -i testsrc2=size=WxH:rate=R -t 4` encoded
to prores_ks / ffv1 / liboapv / libvvenc / libx264 / libwebp_anim / gif;
the timecode burn-in makes orientation and frame position readable in
captures. Notes: `--playlist-test` and `--simulate-user` skip the
single-instance server (a second `qcview.exe <file>` starts a NEW
instance); bin rows load on DOUBLE-click; a running instance holds the
exe (LNK1104 on rebuild).
