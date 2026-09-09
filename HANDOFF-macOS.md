# macOS handoff — after Windows v2.3.0 (2026-09-09)

Written on the Windows box at the end of the FFmpeg 9.0 / hardware-decode
programme. Windows 2.3.0 is packaged (Store + signed sideload, both
verified). This file is the cross-machine recap; the per-commit detail is
in `git log 2608b554..066e4ffc` (23 commits).

## What Windows shipped in 2.3.0

| Area | Change | Commits |
|---|---|---|
| FFmpeg | 9.0.1 self-built (BtbN recipe) with patches 0001 (DNxHR ACT), 0002 (MXF range), **0003 (ProRes RAW Bayer patterns, new)** | `2608b554`, `a7c952c8`, `56a6322d` |
| Vulkan ProRes | App-owned cached frame pools, one decode thread, compositor waits/signals AVVkFrame semaphores, internally synchronised queues (`VK_KHR_internally_synchronized_queues`); root cause of the 2.2.8 device loss was our own pool churn | `956eba82`, `7997945b`, `3a265f7e` |
| D3D11VA | Zero-copy: decode into a texture array on the renderer's ID3D11Device, per-slice SRVs, compute YUV→RGBA16F. H.264/HEVC/AV1/VP9 files and live SRT | `549f0513`, `ef34a6ea` |
| Routing | Vulkan = ProRes only (FFV1/APV measured 6–20× slower than CPU on the 5090), D3D11VA = inter codecs, software = everything else; get_format only accepts the attached device's format | `aa17d374`, `1e9a4ca6`, `24b927a8` |
| CPU path | >8-bit and Bayer sources publish `QImage::Format_RGBA64` → UNORM16 slot (integer, not half); legal-range expansion for RGB in 16-bit | `a80a8931`, `7b88b7e6` |
| swscale | Dynamic-mode `sws_scale_frame`, threaded (`decode/sws_threaded.h`); Bayer stays single-threaded (slice edges break the demosaic); parity harness `QCV_SWS_LEGACY=1` + `QCV_DUMP_FRAME=<dir>` | `90ccd44c` |
| Threading | Intra codecs get slice threads (`decode/thread_policy.h`): ProRes RAW first frame 1.8 s → 24 ms; scrub + dual decoders were single-threaded (libavcodec default) and now aren't; dual = cores/2 per side | `60a537d1`, `e477d24e` |
| Dual view | Chase mode + nearest-frame fallback for sources slower than the playhead; ProRes back on Vulkan for both sides (`QCV_DUAL_PRORES_SW=1` restores the old software workaround) | `e477d24e`, `b21d6780` |
| Rotation | Display rotation (iPhone RAW carries a 180° matrix) re-pushed on playlist clip changes and after loads; failed loads clear the A slot | `7f09a149` |
| ProRes RAW | Software decode (Vulkan RAW hwaccel is flipped + 12-bit upstream), Inspector reports 12-bit / bayer_rggb16le, stale caches re-extracted | `56a6322d`, `ae7e673a` |
| WebP / GIF | Animated WebP (9.0 `webp_anim`) and GIF as videos; `decode/stream_extent.h` counts packets for duration-less containers; `decode/seek_compat.h` reopens `webp_anim` instead of seeking (its seek is broken) | `a776ba51`, `cb2799a1` |
| Docs / pins | **Deliberately untouched**: `dependencies.md` pin table, notices, changelog entries for 9.0 — the macOS cut-over owns those | — |

## What macOS has to do

### 1. FFmpeg 9.0.1 with the three patches
Build 9.0.1 with `external/patches/ffmpeg/0001`, `0002`, `0003`. Then the
pin table / §7 / notices / `dependencies-changelog.md`. Verify the DNxHR
parity recipe (framemd5 `d0a389c3…`, `-cpuflags 0` on x86 only) still
holds on 9.0.1 — it did on Windows.

### 2. The one change that can BREAK macOS as-is: 16-bit CPU frames
`VideoDecoder::publishCpuFrame` now publishes `QImage::Format_RGBA64`
for any >8-bit or Bayer source on **every platform** (`decode/rgb_range.h`,
`cpuPublishPixelFormat` is not gated). The Metal video slot expects
RGBA8. Either teach the Metal CPU slot to upload RGBA64 as
`MTLPixelFormatRGBA16Unorm` (the texture pool already has format code 3
for deep-colour PNG — reuse it) or gate the publish to RGBA on macOS
until then. Affected on Mac: DNxHR 10/12-bit, ProRes when VideoToolbox
declines, ProRes RAW, FFV1/APV/VVC software. The dual path IS gated
(`dual_video_decoder.cpp`: non-Windows uses RGBA8) — mirror whichever
choice you make there. ScrubDecoder / thumbnails still produce RGBA8.

### 3. Compile-untested files touched here
- `src/dual/dual_scrub_decoder_macos.cpp` — swsCreateThreaded()/
  swsFrameToRgbaImage(…, rangeOverride) and seekStream(); mechanical,
  mirrors the Windows scrub decoder.
- `src/decode/thread_policy.h` — applied before every `avcodec_open2`,
  including under VideoToolbox (harmless: the hwaccel decodes; only the
  software fallback sees it).
- `src/decode/sws_threaded.h`, `stream_extent.h`, `seek_compat.h` —
  header-only, plain FFmpeg API.

### 4. Verify on Metal
- Run the swscale parity harness on Mac: `QCV_SWS_LEGACY=1` and unset,
  each with `QCV_DUMP_FRAME=<dir>`, open the same clips, `ffmpeg -lavfi
  psnr` / md5 the PNG pairs. Windows: 6/6 identical (RAW after the
  Bayer single-thread rule).
- Animated WebP: open, play, loop, scrub; the reopen-to-seek path is
  what makes it work.
- Rotation: a 180°-tagged clip in a playlist and after a failed load
  (`-display_rotation 180` on any clip makes a test file).
- Dual: two ProRes clips (VideoToolbox), an 8K-class clip on B to see
  chase mode / nearest-frame fallback keep B moving.
- ProRes RAW: Inspector 12-bit, upright, ~30 ms/frame software.
- Still pending from earlier handoffs: Metal generation-race port
  (`9bc0fea3` mirror), rotation feature on Metal, Metal Dark Gray
  #161616, dual audio via CoreAudio.

### 5. Then build + release macOS 2.3.0
Signed/notarized dmg + Sparkle appcast (previous macOS appcast entry:
2.2.x per the Mac session; there was no 2.2.9 on either platform).

## Windows-only (no Mac action)
Zero-copy D3D11VA, Vulkan pools/semaphores/queues, Vulkan-in-dual,
WASAPI, MSIX. Escape hatches: `QCV_NO_INTERNAL_QUEUE_SYNC=1`,
`QCV_DUAL_PRORES_SW=1`, `QCV_SWS_THREADS=n`.

## Open on both platforms
- GPU ProRes RAW (compositor debayer + the upstream flip/range fix).
- swscale unstable x86 "ops" backend: ~15× faster per thread for 8K
  12-bit YUV→RGBA64; watch each FFmpeg release for it leaving
  `SWS_UNSTABLE`, then adopt env-gated (Bayer not covered).
- Variable-delay WebP/GIF: frame counter assumes a constant rate.
- Still `.webp` lands in Videos (no WebP loader in the still cache).

---

## macOS status (2026-09-09, Mac session)

| Handoff item | Result |
|---|---|
| 1. FFmpeg 9.0.1 + 0001/0002/0003 | Built into `external/install/` with the unchanged recipe; 8.1.2 dylibs parked in `external/install/lib/parked-ffmpeg-8.1.2/`, source in `external/source/ffmpeg-8.1.2-parked/`. Avid ACT vector framemd5 `d0a389c3f22b` (1 & 8 threads) identical to 8.1.2; DNxHR/MXF vectors identical. Pin table, §7, revision history, notices (`FFmpeg.GPL.Shared.9.0`, shaderc/glslang marked build-time only) and `dependencies-changelog.md` updated. `build/` reconfigured against it (`-U 'FFMPEG*'` needed — pkg-config results are cached). |
| 2. 16-bit CPU frames on Metal | Taught the Metal CPU video slot (`uploadCpuFrameRgba`, shared by sides A and B) to upload `Format_RGBA64` as `MTLPixelFormatRGBA16Unorm`; texture cache keys on pixel format too. Dual decoder RGBA64 publish un-gated (`dual_video_decoder.cpp`). Validation-layer run (`MTL_DEBUG_LAYER=1`) on the 12-bit ACT clip: clean. |
| 3. Compile-untested files | One break: `qcv::firstSoftwareFormat` was declared/defined only under `Q_OS_WIN` but called from every platform's `get_format` → now a platform-neutral inline in `vulkan_hw_device_ctx.h`. `dual_scrub_decoder_macos.cpp`, `thread_policy.h`, `sws_threaded.h`, `stream_extent.h`, `seek_compat.h` compile clean; zero FFmpeg deprecation warnings. |
| 4. swscale parity | `QCV_SWS_LEGACY=1` vs unset, `QCV_DUMP_FRAME`: 4/4 byte-identical PNGs — DNxHR LB 8-bit (RGBA8), DNxHR HQX 10-bit (RGBA64), Avid DNxHR 444 12-bit ACT (`gbrp12le` → RGBA64), animated GIF (RGBA8). |
| 4. Visual checks | Pending chris: rotation (`prores_rot180.mov` test vector, playlist + failed load), dual (VideoToolbox ProRes ×2, 8K-class on B), animated WebP (no animated WebP on this Mac — needs a sample), ProRes RAW (no RAW clip on this Mac — needs an iPhone sample). Rotation is implemented on Metal (`setRotationA/B` → compositor quarter-turns) so the pending "rotation feature on Metal" item from the older handoff is already closed. |
| 5. Release | Next: `./scripts/sign-and-notarize.sh build` → GitHub release v2.3.0 → `update_appcast.sh`. Not run yet (waiting on the visual pass). |

Still open from older handoffs (unchanged): Metal generation-race port (`9bc0fea3` mirror), Metal Dark Gray #161616, dual audio via CoreAudio.
