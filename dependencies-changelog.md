# Dependencies Changelog

Per `dependencies.md` §6, every dependency bump is logged here: old → new
pin, reason, tested platforms, and anything to watch in production.

---

## 2026-09-09 — FFmpeg `n8.1.2` → `n9.0.1` (both platforms; three local patches)

**Dependency:** FFmpeg. **Old → new:** `n8.1.2` (8.1 line, two patches)
→ `n9.0.1` (9.0 line, patches 0001 DNxHR ACT, 0002 MXF RGBA range,
**0003 ProRes RAW Bayer patterns — new**). Every library major bumps:
libavutil 61, libavcodec 63, libavformat 63, libavdevice 63,
libavfilter 12, libswscale 10, libswresample 7.

**Windows (2026-09-08, shipped in 2.3.0 — Store + signed sideload):**
BtbN recipe on `release/9.0` with all three patches; vendored at
`external/ffmpeg-win64/`, the 8.1.2 tree parked as
`external/ffmpeg-win64-8.1.2/`. Went with a full hardware-decode
programme (`git log 2608b554..066e4ffc`): app-owned cached Vulkan frame
pools + `VK_KHR_internally_synchronized_queues` (root cause of the 2.2.8
device loss was our own pool churn), zero-copy D3D11VA for inter codecs
and live SRT, 16-bit CPU path with threaded `sws_scale_frame`, slice
threading for intra codecs, animated WebP/GIF, ProRes RAW in software.

**macOS (2026-09-09):** `external/install/` rebuilt from `n9.0.1` +
0001/0002/0003 with the unchanged configure recipe; 8.1.2 dylibs parked
in `external/install/lib/parked-ffmpeg-8.1.2/`, source in
`external/source/ffmpeg-8.1.2-parked/`. Verified: Avid ACT vector
framemd5 `d0a389c3f22b` identical to 8.1.2; DNxHR/MXF regression vectors
identical; app links `libav*.63`/`libavutil.61`, zero FFmpeg deprecation
warnings. macOS-side code needed for the Windows 2.3.0 changes: Metal CPU
video slot uploads `Format_RGBA64` as `RGBA16Unorm` (>8-bit and Bayer
sources keep their depth into the OCIO pass), `firstSoftwareFormat`
made platform-neutral (was Windows-only, called everywhere), dual
decoder RGBA64 publish un-gated.

**macOS ProRes RAW (found after the first 2.3.0 build, release withdrawn and rebuilt):** 9.0's new `prores_raw_videotoolbox` hwaccel was picked by every macOS get_format and VideoToolbox returned null buffers for every frame (-12905), so RAW clips showed nothing. `decode/hw_routing.h` now forces software for ProRes RAW on every platform and all macOS get_format fallbacks return the first software format (fmts[0] is the compiled-in Vulkan hwaccel for RAW under 9.0).

**Watch:** first releases on a fresh major — track 9.0.x point releases
(security fixes now land on 9.0 and 8.1 both). `lock_queue` callbacks
are gone from our code path (internally synchronised queues) so the
libavutil-62 removal no longer matters. `swscale` unstable x86 "ops"
backend still `SWS_UNSTABLE`. Rollback = un-park the 8.1.2 trees and
rebuild (the app source is dual-version).

---

## 2026-09-08 — FFmpeg 9.0 investigation (branch `ffmpeg-9`) — NOT a bump, pin stays `n8.1.2`

**Dependency:** FFmpeg (both platforms). **Status:** investigated and
trial-built on macOS; nothing shipped, `external/install/` and
`external/ffmpeg-win64/` untouched.

**What 9.0 means for us:** every library major bumps (libavutil 61,
libavcodec 63, libavformat 63, libavdevice 63, libavfilter 12,
libswscale 10, libswresample 7). Exactly one app source break
(`av_opt_set_int_list` + abuffersink `sample_fmts`/`sample_rates`/
`ch_layouts`, removed) and one build-system break (literal
`avcodec-62.dll` staging list). Both fixed on the branch in a way that
still builds against 8.1. `lock_queue`/`unlock_queue` (our Vulkan queue
serialization, v2.2.8) are deprecated but still honoured in 9.0; they
disappear at libavutil 62 — forward plan recorded in `dependencies.md`.

**Patches:** both apply cleanly to `n9.0.1`. Patch 0001 regenerated to
also sync `avctx->sw_pix_fmt` at end-of-frame — 9.0's
`avcodec_parameters_from_context()` prefers `sw_pix_fmt`, which held the
patch's provisional header-time label, so stream probes reported
`gbrp10le` for YCbCr-native DNxHR 444. Frames were always correct;
8.1.2 behaviour unchanged. Regression vectors framemd5-identical
8.1.2 vs 9.0.1, and the Avid ACT clip verifies on 9.0.1 (`gbrp12le,tv`,
framemd5 `d0a389c3f22b`, 1 and 8 threads).

**Trial build:** `n9.0.1` + patches → `external/install-ff9/`
(gitignored, same configure flags, no new build deps); app built in
`build-ff9/` via new `-DQCV_FFMPEG_PREFIX`. Links, `probe-video` /
`probe-metadata` match the 8.1.2 build.

**Windows:** official BtbN 9.0 gpl-shared build inspected — Vulkan
ProRes/H.264/HEVC/AV1 hwaccels present with shaders precompiled (no
libshaderc/glslang runtime any more). Rebuild with the patched BtbN
recipe (`./build.sh win64 gpl-shared 9.0`) is the next step there.

**Watch:** `swscale` rewrite is opt-in (legacy API stays on the legacy
backend) — no perf change expected until we migrate to
`sws_scale_frame()` + `SWS_UNSTABLE`. ffmpeg CLI dropped `-vsync` & co.
(qcbridge command lines to audit).

---

## 2026-08-20 — Windows FFmpeg: prebuilt `n8.1.2-20260624` → self-built BtbN recipe `n8.1.2-44-g7c533d0f86-20260820` (local patches)

**Dependency:** FFmpeg, Windows vendored tree `external/ffmpeg-win64/`
(gitignored).

**Pin:** `n8.1.2-44-g7c533d0f86-20260820` = BtbN `release/8.1` head as
of 2026-08-20, built with BtbN's own Docker recipe (ghcr.io toolchain
image, identical configure flags, DLL majors 62/60/62/11/62/6/9 — same
ABI as the retired official zip) **plus the two tracked local patches**
in `external/patches/ffmpeg/`:

- `0001-dnxhd-adaptive-colour-transform.patch` — Avid DNxHR 444 ACT
  decode (fixes green/magenta MB speckle + green border; output becomes
  `gbrp10/12`) and, v2, untagged DNx 4:4:4 stamped
  `color_range = limited` (Avid convention; explicit tags win).
- `0002-mxfdec-rgba-component-ref-color-range.patch` — MXF RGBA
  descriptor ComponentMax/MinRef → `color_range` (RGB essence probes
  full/limited instead of unknown).

**Reason:** upstream carries neither patch; the official prebuilt can't.
macOS has shipped both since v2.2.6/2.2.7 — this brings Windows to
parity for the 2.2.7 release.

**Build:** WSL2 Ubuntu + Docker on the uniongraphics box; full recipe
(incl. the build.sh patch-hook injection and the CRLF + WSL-session
gotchas) in `dependencies.md` §Windows. Patches `git apply --check`
clean on the `release/8.1` head.

**Tested (Windows, 2026-08-20):** Avid ACT clip probes `gbrp12le` +
`color_range=tv`, decodes clean (no speckle/green border);
`-cpuflags 0` framemd5 identical to the macOS arm64 build
(`d0a389c3f22b…`; default x86 SIMD differs only by pre-existing
upstream IDCT rounding). FFmpeg-encoded DNxHR 444 (`yuv444p10` +
`gbrp10`) decodes bit-identical to the stock prebuilt. MXF RGBA
ComponentRef edit flips probe pc↔tv. `-buildconf` retains
libplacebo/libshaderc/vulkan (ProRes Vulkan path intact). App runtime
verified in QCView 2.2.7 (CPU publish path, RGB legal-range expansion
ON under Auto). Shipped in the 2.2.7 MSIX pair.

**Watch:** every future refresh must re-apply both patches (stock zip
silently regresses); the framemd5 cross-platform check needs
`-cpuflags 0` on x86.

---

## 2026-07-02 — SoundTouch: NEW dependency, commit `0047e0b1` (tag 2.4.1)

**Dependency:** SoundTouch (vendored via FetchContent in
`external/CMakeLists.txt`, statically linked into `qcv_audio`).

**Pin:** commit `0047e0b1ecfceb041348579119bf79b73a322a3a` = upstream
release tag `2.4.1` (codeberg.org/soundtouch/soundtouch).

**Reason:** New feature — constant-pitch review-speed playback
(0.5x–2x). `TempoStage` (`src/audio/tempo_stage.{h,cpp}`) wraps one
`soundtouch::SoundTouch` per audio decoder as a decode-thread WSOLA
stage; 1x playback bypasses it entirely.

**Build flags:** `INTEGER_SAMPLES=OFF` (float pipeline),
`SOUNDSTRETCH=OFF`, `SOUNDTOUCH_DLL=OFF`. NEON autodetects on arm64.

**License:** LGPL v2.1, statically linked — covered by the app's GPL v3
distribution (see `LICENSES/THIRD_PARTY_NOTICES.txt` §36 +
`LICENSES/SoundTouch-LICENSE.txt`).

**Tested:** macOS arm64 (builds + links clean). Windows build pending
next Windows CI/machine pass — plain C++ with first-class CMake, no
platform code expected to differ.

---

## 2026-06-24 — macOS FFmpeg: self-built `n8.1` → `n8.1.2`

**Dependency:** FFmpeg (macOS self-built arm64 build, vendored in-tree at
`external/install/`).

**Old pin:** `n8.1` (self-built, libavcodec 62.28.100 / libavutil
60.26.100 / libavformat 62.12.100), built ~May 2026.

**New pin:** `n8.1.2` (self-built, libavcodec 62.28.102 / libavutil
60.26.102 / libavformat 62.12.102).

**Reason:** Security — same **CVE-2026-8461 "PixelSmash"** (CVSS 8.8) heap
out-of-bounds write in libavcodec's MagicYUV slice decoder fixed in the
Windows entry below. The macOS build is a self-built `n8.1` binary that
**predated** FFmpeg 8.1.2 (released 2026-06-17) and was therefore
vulnerable — contrary to the earlier (now corrected) claim that macOS was
unaffected. macOS ships this self-built FFmpeg, **not** Homebrew (the
`external/install/lib/pkgconfig` dir is prepended to `PKG_CONFIG_PATH` by
`CMakeLists.txt`).

**How it was rebuilt:** cloned `FFmpeg.git` at tag `n8.1.2`, configured
with the **same flags as the prior build** (replicated verbatim from the
old dylib's `avcodec_configuration()` string) **minus the vestigial
`--enable-nonfree`**, reusing the in-tree static codec archives in
`external/install/lib` (x264/x265/dav1d/vpx/mp3lame/opus/SvtAv1Enc).
`make install` overwrote the `libav*`/`libsw*` dylibs + headers in
`external/install/`; stale old-micro versioned dylibs were pruned. Full
recipe is in `dependencies.md` §2 → "macOS: self-built GPL v3".

**Side fix:** dropped `--enable-nonfree`. No GPL-incompatible codec was
ever linked (AAC is FFmpeg-native LGPL + Apple AudioToolbox), so the flag
did nothing but was an audit red flag. The binary now matches the macOS
entry in `LICENSES/THIRD_PARTY_NOTICES.txt` exactly.

**One pkgconfig addition:** added `external/install/lib/pkgconfig/x265.pc`
(pointing at the in-tree static `libx265.a`). FFmpeg's configure *requires*
pkg-config for `libx265`, and only a Homebrew `x265.pc` (a possibly
different x265 build) existed before — the vendored `.pc` guarantees
configure links the exact in-tree x265.

**Verification (this machine, 2026-06-24):**
- `external/install/include/libavutil/ffversion.h` → `n8.1.2`.
- `avcodec_version()` via dlopen → `62.28.102`; dylib loads cleanly.
- Sonames unchanged (libavcodec 62 / libavutil 60 / libavformat 62) →
  ABI-clean drop-in, no CMake / source changes needed.
- `avcodec_configuration()` confirms all codecs present and **no**
  `--enable-nonfree`.
- External non-system deps unchanged from the prior build (still just
  `/opt/homebrew/.../libX11.6.dylib`, handled by `bundle_dylibs.sh`).
- App reconfigured + built clean (`build/src/app/qcview.app`) against the
  new headers/dylibs.

**Tested platforms:** macOS (built + linked + dylib load verified). A
runtime smoke test (open a clip, scrub, ProRes/H.264 decode) before
shipping the signed DMG is still recommended.

**Watch for in production:** none expected — micro-version bump on the
same 8.1 branch, identical configure surface. Confirm the next
`sign-and-notarize.sh` run bundles the new dylibs (it walks deps from the
executable, so the rebuilt `external/install/lib` libs are picked up
automatically).

---

## 2026-06-24 — Windows FFmpeg: `n8.1-11-g75d37c499d` → `n8.1.2-20260624`

**Dependency:** FFmpeg (Windows GPL-Shared build from
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds))

**Old pin:** `ffmpeg-n8.1-11-g75d37c499d-win64-gpl-shared-8.1`
(BtbN release `autobuild-2026-04-30-13-44`, consumed via the
`BtbN.FFmpeg.GPL.Shared.8.1` winget package).

**New pin:** `n8.1.2-20260624` — BtbN rolling `latest` release asset
`ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip`, now **vendored in-tree** at
`external/ffmpeg-win64/` (gitignored; see below).

**Reason:** Security — **CVE-2026-8461 "PixelSmash"** (CVSS 8.8): a heap
out-of-bounds write in libavcodec's MagicYUV slice decoder, where the
frame allocator and decoder disagree on chroma plane height. A crafted
AVI/MKV/MOV can crash or (with a refined chain) RCE. Fixed upstream in
FFmpeg **8.1.2** (released 2026-06-17). The prior `n8.1-11` build predated
the fix and was vulnerable.

**Why not winget:** the `BtbN.FFmpeg.GPL.Shared.8.1` winget package was
still pinned to the vulnerable April build (`8.1-20260430`) with
`winget upgrade` reporting no update available. We consume BtbN's rolling
`latest` GitHub release zip directly instead.

**Why vendored + gitignored:** the build is ~200 MB and `avcodec-62.dll`
alone is ~98 MB. `main` auto-pushes to GitHub, which hard-blocks files
≥100 MB — committing the binaries risks breaking the push entirely on any
future size bump. Files live in the repo tree for the build to find but
are excluded via `.gitignore` (`external/ffmpeg-win64/`); re-fetch
instructions are in `dependencies.md` §2.

**Verification (uniongraphics box, 2026-06-24):**
- `ffmpeg -version` → `ffmpeg version n8.1.2-20260624`
- `ffmpeg -buildconf` confirms `--enable-vulkan`, `--enable-libshaderc`,
  `--enable-libplacebo` (the reason we use BtbN over vcpkg — ProRes Vulkan
  compute decode) and `--enable-gpl`.
- Library majors unchanged from the old build: **libavcodec 62 /
  libavutil 60 / libavformat 62 / libavfilter 11 / libavdevice 62 /
  libswresample 6 / libswscale 9** — matches the DLL names copied in
  `src/app/CMakeLists.txt`, so it's an ABI-clean drop-in (no code or
  DLL-list changes needed).
- `.pc` files are relocatable (`prefix=${pcfiledir}/../..`), so the tree
  resolves correctly from `external/ffmpeg-win64/`.

**CMake wiring changes:**
- `external/CMakeLists.txt`: default `QCV_BTBN_FFMPEG_DIR` changed from the
  winget cache path to `${CMAKE_SOURCE_DIR}/external/ffmpeg-win64`, now
  `FORCE`-set when empty/unset (the `windows-release` preset caches the
  var as `""` when `BTBN_FFMPEG_DIR` is unset, and a non-FORCE `set` can't
  override an existing empty cache entry). Fallback warning updated.
- `.gitignore`: added `external/ffmpeg-win64/`.

**Tested platforms:** Windows (verified the build's version + buildconf;
full configure/build + ProRes Vulkan-decode smoke test still recommended
before shipping). **macOS was ALSO affected** (it ships a self-built
`n8.1` FFmpeg, not Homebrew — an earlier draft of this note wrongly said
otherwise) and is addressed in the next entry below.

**Watch for in production:** confirm ProRes Vulkan compute decode still
works end-to-end after the bump (libplacebo/libshaderc are present, but
exercise an actual ProRes clip). Re-verify after any future BtbN refresh —
the rolling `latest` tag is not version-stamped in the asset name, so
always check `ffmpeg -version` after re-fetching.
