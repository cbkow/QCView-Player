# Dependencies — Pin Manifest

Single source of truth for every external dependency used by
QCView-Player-QT. Every version below is **pinned** — `CMakeLists.txt`
references this file's pin choices via `FetchContent_Declare(... GIT_TAG <pin>)`
or system package version constraints.

## Pin policy

- **Every dep is pinned** to a specific tag or commit hash. Never
  `master` / `main` / `HEAD`. Reproducible builds are non-negotiable.
- **Bump cadence**: deliberate, scheduled. No "latest" upgrades from CI.
  Quarterly review at minimum; security/CVE fixes immediately.
- **Each bump documents**: old version, new version, reason, tested
  platforms. Append to a `dependencies-changelog.md` per bump.
- **`OCIO library version` ceiling** (Guide 05 §12) is derived from the
  pinned OCIO version; ship-time supported config profile listed below.

## Categories

1. **Qt SDK** — installed via Qt's installer or system package; not
   vendored.
2. **FetchContent** — fetched at configure time, built in-tree, pinned
   by Git tag or commit hash.
3. **System libraries** — fixed minimum version requirements; resolved
   via platform package managers or vendored at packaging time.
4. **Bundled assets** — files vendored in the repo (OCIO configs,
   safety-guide SVGs, fonts).

---

## 1. Qt SDK

| Dep | Pin | Source | License | Notes |
|---|---|---|---|---|
| **Qt** | **6.11.0** (latest 6.11.x patch at build) | Qt installer or `qt6` system package | LGPLv3 / commercial | Confirmed installed at `/Applications/Qt/6.11.0` on dev machine. Modules required: `Core`, `Gui`, `Quick`, `QuickControls2`, `Network`, `Widgets` (for QFileDialog only), `Concurrent`, `Multimedia` (audio output only — video pipeline is custom QRhi). |

Required Qt modules (explicit list):

- `Qt6::Core`
- `Qt6::Gui` — for QRhi
- `Qt6::Quick` — Qt Quick scene graph
- `Qt6::QuickControls2` — UI controls
- `Qt6::ShaderTools` — shader compilation pipeline (qsb)
- `Qt6::Network` — Frame.io HTTP client
- `Qt6::Concurrent` — QFuture for worker threads
- `Qt6::Multimedia` — `QAudioSink` for audio output

Build flag: `QT_NO_DEPRECATED_BEFORE=0x060B00` (Qt 6.11) — locks API
surface against accidental use of pre-6.11 deprecations.

---

## 2. FetchContent dependencies

Each entry includes the `FetchContent_Declare()` block to copy into
`external/CMakeLists.txt`.

### FFmpeg

```cmake
FetchContent_Declare(
    ffmpeg
    GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
    GIT_TAG n9.0.1                              # release tag, ABI-stable
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **n9.0.1** (9.0 release branch) since 2026-09-09 — both **macOS (self-built, in-tree at `external/install/`)** and **Windows (self-built BtbN recipe, 9.0 branch, 2026-09-08)** — both carry the three local patches in `external/patches/ffmpeg/` (0001 DNxHR ACT, 0002 MXF RGBA range, 0003 ProRes RAW Bayer patterns) |
| **License** | **GPL v3** on both shipped platforms (macOS self-built `--enable-gpl --enable-version3`; Windows BtbN GPL-Shared `--enable-gpl`) |
| **Verified** | macOS self-built `n9.0.1` (libavcodec 63.1.101 / libavutil 61.1.101 / libavformat 63.1.101 / libswscale 10.1.101) loads + app links/builds clean (2026-09-09); Windows 2.3.0 shipped on the BtbN-recipe 9.0.1 build (Store + sideload) |
| **Build flags** | `--enable-videotoolbox --enable-vulkan --enable-libdav1d --enable-libsvtav1 --enable-libopus --enable-libsrt --disable-x86asm-on-cross` (9.0: Vulkan shaders are precompiled at build time — `glslc`/`glslang` on the build box; no `--enable-libshaderc`) |
| **Hwaccels needed** | `videotoolbox` (macOS), `vulkan` (Win+Linux) |
| **Codecs needed** | H.264, H.265, AV1, ProRes, DNxHR, JPEG (image-sequence fallback) |

Note: building FFmpeg from source via FetchContent is a **slow build**
(~10–15 min on first config). The dev workflow may prefer Homebrew /
distro packages; CI and shipped builds use FetchContent for
reproducibility. CMake handles both via a `QCVIEW_USE_SYSTEM_FFMPEG`
toggle.

#### Windows: BtbN GPL-Shared build (vendored, in-tree)

Windows uses a [BtbN GPL-Shared build](https://github.com/BtbN/FFmpeg-Builds)
(9.0 branch since 2026-09-08). Historically the reason was that vcpkg's
FFmpeg 8.1 shipped without libplacebo + libshaderc, so the ProRes Vulkan
compute decoder was unavailable; with 9.0 the shaders are precompiled
and that dependency is gone, but BtbN's recipe is still what carries
our three local patches. The DLL majors (libavcodec 63 / libavutil 61 /
libavformat 63 / libavfilter 12 / libavdevice 63 / libswresample 7 /
libswscale 10) are derived from the `.pc` versions in
`src/app/CMakeLists.txt`, so the DLL-copy step follows a major bump.

- **Vendored at** `external/ffmpeg-win64/` — the default
  `QCV_BTBN_FFMPEG_DIR`. **Gitignored** (~200 MB; `avcodec-63.dll` alone
  is ~98 MB, and `main` auto-pushes to GitHub which hard-blocks
  ≥100 MB files). The `.pc` files are relocatable
  (`prefix=${pcfiledir}/../..`), so the tree works from any location.
- **Current build:** `n9.0.1` (BtbN `release/9.0`, built 2026-09-08 —
  the exact `n9.0.1-N-g…-20260908` tag is in `ffmpeg.exe -version` on
  the Windows box) — **self-built with BtbN's own recipe** (WSL2 +
  Docker, see below) so it carries the three local patches in
  `external/patches/ffmpeg/` (DNxHR 444 ACT, MXF RGBA range, ProRes
  RAW Bayer patterns — see the macOS section for what they do). Same
  toolchain image, same configure flags, same DLL majors as the
  official BtbN zip → byte-level drop-in. Shipped in Windows 2.3.0.
  The previous 8.1-line tree (`n8.1.2-44-g7c533d0f86-20260820`, two
  patches) is parked as `external/ffmpeg-win64-8.1.2/` (gitignored).
- **Why not winget:** the `BtbN.FFmpeg.GPL.Shared.8.1` winget package was
  still pinned to the vulnerable April build (`8.1-20260430`) with no
  upgrade available; and since 2026-08-20 the official prebuilts are out
  entirely — they don't carry our local patches.
- **Override** with the `BTBN_FFMPEG_DIR` env var (bound by the
  `windows-release` preset) or `-DQCV_BTBN_FFMPEG_DIR=<path>`.

**Rebuild / refresh — patched BtbN-recipe build (2026-08-20).** Both
patches in `external/patches/ffmpeg/` **must be re-applied on every
refresh**; a stock BtbN zip loses them (Avid DNxHR 444 ACT speckle
comes back and MXF RGB range probes unknown). Build runs on this box in
WSL2 Ubuntu with Docker inside the distro (`wsl --install -d Ubuntu`,
`apt install docker.io`); the BtbN toolchain image is pulled prebuilt
from ghcr.io, so a full build is ~5 min on 32 threads:

```bash
# inside WSL2 Ubuntu (root), one-time setup:
git clone https://github.com/BtbN/FFmpeg-Builds.git ~/FFmpeg-Builds
mkdir -p ~/FFmpeg-Builds/ffpatches

# every refresh: copy CURRENT patches in (strip the CRLF the Windows
# checkout adds — git apply fails on CRLF patches with `?`-suffixed
# context errors):
cp /mnt/c/Users/<you>/Documents/GitHub/QCView-Player/external/patches/ffmpeg/*.patch ~/FFmpeg-Builds/ffpatches/
sed -i 's/\r$//' ~/FFmpeg-Builds/ffpatches/*.patch

# one-time: inject the patch step into build.sh — BtbN has no FFmpeg
# patch hook (its patches/ dir is deps-only). Two edits:
#   1. in the BUILD_SCRIPT heredoc, right after `cd ffmpeg`:
#        for _p in /ffpatches/*.patch; do
#            git apply --verbose "\$_p"       # note the \$ escape
#        done
#   2. add `-v "$PWD/ffpatches":/ffpatches` to the `docker run … bash /build.sh` line
cd ~/FFmpeg-Builds && ./build.sh win64 gpl-shared 9.0
# → artifacts/ffmpeg-<ver>-win64-gpl-shared-9.0.zip
# Log must show all THREE "Applied patch … cleanly." lines before configure.
```

Gotcha: WSL kills background work when the last `wsl.exe` session
detaches — run the build inside a session that stays open (or
`cmd /c wsl …` from a persistent Windows process), not `nohup`/
`systemd-run`.

Then replace the vendored tree and verify (from repo root, PowerShell):

```powershell
Remove-Item external\ffmpeg-win64 -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive <artifact>.zip -DestinationPath $env:TEMP\ffx -Force
Move-Item "$env:TEMP\ffx\ffmpeg-<ver>-win64-gpl-shared-8.1" external\ffmpeg-win64
# Verify: 8.1.2+, all three Vulkan deps, and the ACT patch live.
external\ffmpeg-win64\bin\ffmpeg.exe -hide_banner -version | Select-Object -First 1
external\ffmpeg-win64\bin\ffmpeg.exe -hide_banner -buildconf | Select-String "libplacebo|libshaderc|vulkan"
# ACT check: an Avid DNxHR 444 ACT clip must probe pix_fmt=gbrp12le
# (stock says yuv444p12le) and, untagged, color_range=tv.
```

Decode-parity note (2026-08-20): the macOS↔Windows framemd5 cross-check
on an ACT clip matches **with `-cpuflags 0`** on Windows (C IDCT path =
arm64 output). Default x86 SIMD differs by upstream IDCT rounding only —
pre-existing FFmpeg behaviour, not the patches; FFmpeg-encoded DNxHR
444 regression files decode bit-identical to the stock build either way.

(Historical: 2026-06-24 → 2026-08-20 the tree was the official BtbN
`latest` release zip, `n8.1.2-20260624`, fetched per the old
Invoke-WebRequest recipe. That recipe is retired — it can't carry the
local patches.)

#### macOS: self-built GPL v3 (vendored, in-tree)

macOS does **not** use Homebrew for the shipped binary — it links a
**self-built arm64 FFmpeg** installed into `external/install/` (dylibs in
`external/install/lib`, headers in `external/install/include`). CMake
finds it because `CMakeLists.txt` prepends `external/install/lib/pkgconfig`
to `PKG_CONFIG_PATH` (see `QCV_VENDOR_PREFIX`). The whole `external/install/`
tree is **gitignored**, so the binary is never committed and must be
rebuilt from source on each dev machine / version bump.

- **Current build:** `n9.0.1` (since 2026-09-09) with all three local
  patches. Every soname changed from the 8.1 line (libavcodec 63 /
  libavutil 61 / libavformat 63 / libavfilter 12 / libavdevice 63 /
  libswresample 7 / libswscale 10); the 8.1.2 dylibs and source tree
  are parked (`external/install/lib/parked-ffmpeg-8.1.2/`,
  `external/source/ffmpeg-8.1.2-parked/`, both gitignored) for a
  rollback. Prior build: `n8.1.2` (CVE-2026-8461 "PixelSmash" fix).
- **Local patch — DNxHR 444 Adaptive Colour Transform (2026-08-20):**
  `external/patches/ffmpeg/0001-dnxhd-adaptive-colour-transform.patch`
  (tracked). Upstream's `dnxhddec.c` reads the per-macroblock ACT flag
  but never applies a transform, so Avid-encoded DNxHR 444 12/10-bit
  with the header ACT flag (AE / Media Composer "Avid DNxHR Codec"
  exports) decoded with green/magenta macroblock speckle and a green
  border. The patch records each MB's flag and, when a frame mixes
  modes, converts the YCbCr MBs (full-range BT.709, planes Y/Cb/Cr) and
  re-orders the RGB MBs (planes R/G/B) into one `gbrp10/12` picture;
  all-transformed frames (what FFmpeg's own encoder writes) stay
  `yuv444p10/12` bit-identical to upstream. Derived empirically from
  boundary fits on Avid material (see the patch header comment). Also
  (v2.2.7) tags untagged 4:4:4 output `color_range = limited` — Avid's
  legal-range convention, mirroring what the ProRes decoder does for
  its own; explicit container tags win.
  **Re-apply after every FFmpeg clone** (step in the recipe below).
  Windows carries it too since 2026-08-20 (v2.2.7) via the patched
  BtbN-recipe build — see the Windows section above.
- **Local patch — MXF RGBA descriptor range (2026-08-20):**
  `external/patches/ffmpeg/0002-mxfdec-rgba-component-ref-color-range.patch`
  (tracked). Upstream mxfdec maps CDCI Black/White ref levels to
  `color_range` but ignores the RGBA descriptor's ComponentMinRef /
  ComponentMaxRef (0x3407/0x3406), so RGB essence in MXF (Avid DNxHR
  444 RGB masters) always probed "unknown". The patch reads them:
  0 / 2^n−1 → full, 16·2^(n−8) / 235·2^(n−8) → limited. Feeds the
  Inspector's Range-tag row and the Range pill's Auto. Windows carries
  it too since 2026-08-20 (v2.2.7), same as above.
- **Codec deps** are pre-built **static** archives already in
  `external/install/lib` (`libx264/libx265/libdav1d/libvpx/libmp3lame/`
  `libopus/libSvtAv1Enc.a`) with matching `.pc` files in
  `external/install/lib/pkgconfig` (note: `x265.pc` is vendored there too,
  since FFmpeg's configure requires pkg-config for libx265).

**Re-build** (run from repo root; reuses the in-tree static codec deps,
overwrites the `libav*`/`libsw*` dylibs + headers in `external/install/`):

```bash
INSTALL="$PWD/external/install"
git clone --depth 1 --branch n9.0.1 https://github.com/FFmpeg/FFmpeg.git external/source/ffmpeg
cd external/source/ffmpeg
git apply ../../patches/ffmpeg/0001-dnxhd-adaptive-colour-transform.patch   # DNxHR 444 ACT fix (see note above)
git apply ../../patches/ffmpeg/0002-mxfdec-rgba-component-ref-color-range.patch  # MXF RGBA range tag (see note above)
git apply ../../patches/ffmpeg/0003-prores-raw-bayer-patterns.patch          # ProRes RAW Bayer patterns (see note above)
PKG_CONFIG_PATH="$INSTALL/lib/pkgconfig" ./configure \
  --prefix="$INSTALL" --enable-shared --disable-static --enable-pthreads \
  --enable-videotoolbox --enable-audiotoolbox \
  --enable-hwaccel=h264_videotoolbox --enable-hwaccel=hevc_videotoolbox \
  --enable-hwaccel=prores_videotoolbox \
  --enable-gpl --enable-version3 \
  --enable-libx264 --enable-libx265 --enable-libdav1d --enable-libvpx \
  --enable-libmp3lame --enable-libopus --enable-libsvtav1 \
  --enable-libsrt \
  --disable-doc --disable-debug --arch=arm64 \
  --extra-cflags="-I$INSTALL/include -mmacosx-version-min=13.0 -arch arm64" \
  --extra-ldflags="-L$INSTALL/lib -mmacosx-version-min=13.0 -arch arm64" \
  --extra-libs=-lc++
make -j"$(sysctl -n hw.ncpu)" && make install
# make install leaves the old versioned dylibs behind — park or prune stale
# ones (a major bump also leaves dangling libavcodec.<oldmajor>.dylib symlinks):
#   ls external/install/lib/libav*.*.*.dylib  (keep only the newest micro)
# Verify: cat external/install/include/libavutil/ffversion.h  → n9.0.1
#         external/install/bin/ffmpeg -protocols | grep srt   → srt in+out
```

**NOTE — no `--enable-nonfree`:** the previous build carried a vestigial
`--enable-nonfree` (no GPL-incompatible codec was ever linked). It was
dropped in this rebuild; the binary now matches the
`LICENSES/THIRD_PARTY_NOTICES.txt` macOS entry exactly.

**NOTE — `--enable-libsrt` + programs (2026-07-26, v2.2.3):** the SRT
protocol was added for the live `srt://` MediaItem (Blender-bridge
stage 2), and `--disable-programs` was dropped so the build also produces
the `ffmpeg` CLI — QCView ships it and acts as the suite's ffmpeg
provider (see `toolbox.json`). avfoundation input devices ride along via
libavdevice (autodetected on macOS) for the qcbridge mac-replica encoder
fallback.

#### macOS: libsrt 1.5.6 + mbedTLS 3.6.7 (vendored static, SRT transport)

Prerequisite for `--enable-libsrt`. Both are static archives in
`external/install/lib` (same pattern as the codec deps). mbedTLS
(Apache-2.0, GPLv3-compatible) is the crypto backend for SRT's AES
passphrase support — chosen over OpenSSL for build weight, over GnuTLS
for CMake-native static builds.

```bash
INSTALL="$PWD/external/install"
# mbedTLS — use the RELEASE TARBALL, not git (git needs the framework
# submodule + Python codegen; the tarball ships pre-generated files)
cd external/source
curl -sL -o mbedtls.tar.bz2 https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
tar xjf mbedtls.tar.bz2 && mv mbedtls-3.6.7 mbedtls && rm mbedtls.tar.bz2
cd mbedtls
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INSTALL" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
cmake --build build -j"$(sysctl -n hw.ncpu)" && cmake --install build

# libsrt
cd .. && git clone --depth 1 --branch v1.5.6 https://github.com/Haivision/srt.git srt
cd srt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INSTALL" \
  -DCMAKE_PREFIX_PATH="$INSTALL" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_APPS=OFF -DUSE_ENCLIB=mbedtls
cmake --build build -j"$(sysctl -n hw.ncpu)" && cmake --install build
```

**Post-install fix-up (required):** FFmpeg's configure calls plain
`pkg-config` (no `--static`), so the mbedTLS archives listed in
`srt.pc`'s `Libs.private` never reach the `libavformat` link. Fold them
into `Libs` in both `external/install/lib/pkgconfig/srt.pc` and
`haisrt.pc` (append the three `libmbed*.a` absolute paths + `-lc++` to
the `Libs:` line, blank out `Libs.private:`). Verify the closure before
running FFmpeg's configure:
`cc test.c $(pkg-config --cflags --libs srt)` with
`PKG_CONFIG_PATH=external/install/lib/pkgconfig` must link and run a
`srt_startup()`/`srt_cleanup()` pair.

#### FFmpeg 9.0 — migration status (investigated 2026-09-08 on branch `ffmpeg-9`; ADOPTED — Windows 2.3.0 shipped 2026-09-08, macOS cut over 2026-09-09)

FFmpeg 9.0 "Lei" (2026-08-04; point release `n9.0.1`) bumps **every**
library major — libavutil 61 / libavcodec 63 / libavformat 63 /
libavdevice 63 / libavfilter 12 / libswscale 10 / libswresample 7 — so
all sonames and DLL names change. The pin above is now `n9.0.1` on both
platforms; the *Before adopting* list at the end records how each step
closed. Everything below was verified against real `n9.0.1` builds.

**App-side breaking changes (exhaustive sweep of `src/`, `tools/`, CMake):**

1. `av_opt_set_int_list()` and abuffersink's legacy list options
   (`sample_fmts` / `sample_rates` / `ch_layouts`) are gone
   (`FF_API_OPT_INT_LIST`, `FF_API_BUFFERSINK_OPTS`). The one caller,
   `src/audio/multi_stream_audio_decoder.cpp`, now uses
   `av_opt_set_array()` on the array-typed `sample_formats` /
   `samplerates` / `channel_layouts` options — present since FFmpeg 7.1,
   so the same code builds against 8.1 and 9.x.
2. `src/app/CMakeLists.txt` staged `avcodec-62.dll` etc. by literal
   name. The list is now derived from the pkg-config module versions
   (`FFMPEG_libavcodec_VERSION` → `avcodec-<major>.dll`), so a BtbN
   refresh across a major bump needs no edit.
3. Nothing else needed touching. Verified unchanged for what we use:
   the legacy stateful swscale API (`sws_getContext` / `sws_scale` /
   `sws_setColorspaceDetails`; the `SWS_BILINEAR` / `SWS_POINT` flags
   moved to a "deprecated in favour of `SwsContext.scaler`" block but
   carry no `attribute_deprecated`), `hwcontext_d3d11va.h` and
   `hwcontext_videotoolbox.h` (0 changed lines), the `AVVkFrame.img[]`
   / `AVVulkanFramesContext.format[]` fields the D3D11 bridge reads,
   `codecpar->coded_side_data` + `av_display_rotation_get`, the
   `AVChannelLayout` / `swr_alloc_set_opts2` audio path, `avfft.h`
   removal (unused), and the removed `FF_CODEC_PROPERTY_*`,
   `AVCodec->pix_fmts`, `av_opt_ptr`, `AVPacketList` (all unused).
   Zero FFmpeg deprecation warnings in the app build.

**Deprecations to plan for (compile clean in 9.0, break later):**

- `AVVulkanDeviceContext.lock_queue` / `unlock_queue` — deprecated in
  lavu 60.29 "without replacement", removed at lavu 62 (FFmpeg 10).
  `src/decode/vulkan_hw_device_ctx.cpp` already guards them with
  `FF_API_VULKAN_SYNC_QUEUES`. In 9.0 they are still honoured:
  `ff_vk_exec_submit()` calls them around every `vkQueueSubmit2`, and
  because `VulkanDeviceManager` does not enable
  `VK_KHR_internally_synchronized_queues` (new in 9.0's optional-extension
  table) libavutil still allocates its own per-queue mutexes and defers
  to our callbacks — so the v2.2.8 app-wide queue serialization keeps
  working. Forward path when they vanish: enable
  `VK_KHR_internally_synchronized_queues` where the driver offers it
  (FFmpeg then skips its own locking and the driver serializes all
  submitters), or give FFmpeg dedicated queue indices via `qf[].num` so
  it never shares a `VkQueue` with the D3D11 bridge / compositor.
- `AVVkFrame.access[]` is now `VkAccessFlagBits2` and the fixed
  queue-family fields (`queue_family_index`, `nb_graphics_queues`, …)
  are removed — we touch neither (we fill `qf[]` / `nb_qf`).

**Vulkan on Windows — what actually changed:** 9.0 dropped the
`libshaderc` / `libglslang` runtime dependency; every Vulkan shader is
compiled to SPIR-V at FFmpeg build time (`glslc` / `glslang` on the
build box, `spirv_compiler` in configure). The official BtbN
`ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip` (2026-09-07) was
downloaded and inspected: `avcodec-63.dll` carries `prores_vulkan`,
`prores_raw_vulkan`, `h264/hevc/av1/vp9/ffv1/apv/dpx_vulkan`,
`h264/hevc_d3d11va`, `h264_d3d12va`, and `swscale-10.dll` has the
SPIR-V backend. So the "vcpkg lacks libplacebo + libshaderc → no ProRes
Vulkan decode" reason for BtbN (§Windows above) dissolves in 9.x; BtbN
is still required for the local patches. Also new: `hwcontext_vulkan`
pixel formats X2RGB10 / X2BGR10 / XV30 / RGBAF16, and
`AVVulkanDeviceContext.queue_flags` (zero-init is fine).

**swscale rewrite — opt-in, not automatic:** the legacy stateful API
"always implies `SWS_BACKEND_LEGACY`", and even `sws_scale_frame()`
picks the legacy backend unless `SWS_UNSTABLE` is set or a float format
is involved (`libswscale/graph.c: prefer_ops_backend`). All six of our
`SwsContext` owners (`video_decoder`, `scrub_decoder`, `live_stream_decoder`,
`video_image_loader`, `dual_video_decoder`, `dual_scrub_decoder{,_macos}`)
use the legacy API, so 9.0 changes nothing for them except the reworked
AArch64 NEON yuv2rgb kernels that live in the legacy path. Getting the
new C / NEON / x86 / SPIR-V backends means migrating those sites to
`sws_scale_frame()` + `SWS_UNSTABLE` (or `SwsContext.backends`) and
measuring bit-exactness (`SWS_BITEXACT`) and throughput — a separate
follow-up, not part of the version bump.

**Local patches on 9.0.1:** both `external/patches/ffmpeg/*.patch`
`git apply --check` cleanly (upstream `dnxhddec.c` is byte-identical
between `n8.1.2` and `n9.0.1`; `mxfdec.c` differs by 3 lines elsewhere).
One behavioural interaction needed a fix: 9.0's
`avcodec_parameters_from_context()` prefers `avctx->sw_pix_fmt` over
`pix_fmt`, and `ff_get_buffer()` mirrors the ACT patch's *provisional*
header-time label into `sw_pix_fmt`, so `avformat_find_stream_info()`
reported `gbrp10le` for a YCbCr-native (FFmpeg-encoded `yuv444p10`)
DNxHR 444 file even though every frame came out `yuv444p10le`. Patch
0001 now syncs `sw_pix_fmt` in its end-of-frame correction (regenerated
2026-09-08; applies to both trees; no effect on 8.1.2, where codecpar
still comes from `pix_fmt`). Verified on 9.0.1: framemd5 identical to
the 8.1.2 build for the regression vectors (`yuv444p10` MOV,
`gbrp10` MOV, `gbrp10` MXF → `pc`, hex-edited RGBA ref levels → `tv`),
probe labels now match 8.1.2. The Avid ACT test vector (CW clip) also
passes on 9.0.1: `gbrp12le,tv`, framemd5 `d0a389c3f22b` at 1 and 8
threads, identical to the 8.1.2 build, no "variable ACT" errors, frame-1
PNG byte-identical.

**Local patch 0003 — ProRes RAW Bayer patterns (2026-09-08, Windows-verified):**
`external/patches/ffmpeg/0003-prores-raw-bayer-patterns.patch`. Upstream
(9.0.1 and master) rejects every ProRes RAW frame whose header Bayer
pattern is not 0 ("Bayer pattern N is not implemented"); an iPhone 17 Pro
clip carries 3. The decoded mosaic is laid out identically regardless
of the value — that clip debayers correctly ONLY as RGGB (BGGR inverts
the hues, GRBG/GBRG go grey) — so the patch accepts every value, keeps
the RGGB tag and logs the raw value at verbose. It also registers the
other three 16-bit Bayer formats in hwcontext_vulkan's format table.
The app routes `prores_raw` to software decode + swscale debayer
(1.2 fps at 4224x3024; viewable, not real-time). The 9.0.1 Vulkan
ProRes RAW hwaccel decodes the same clip at ~158 fps but its output is
vertically flipped and left in a 12-bit range (both also in master as
of May 2026) — needs an upstream-style fix plus a compositor debayer
mode before it can be used. Re-apply on every FFmpeg refresh like
0001/0002 (the recipes' "both patches" now means all three).

**macOS trial build (done):** `n9.0.1` + patches built with the recipe
above, unchanged flags, into `external/install-ff9/` (gitignored) so
`external/install/` and `main` stay intact. Build tooling needed nothing
new (no `--enable-libshaderc`; with Homebrew Vulkan headers 1.4.350 +
`glslc` present configure also compiled the Vulkan hwaccels and SPIR-V
shaders, exactly as 8.1.2 already autodetected `vulkan`; no new dylib
link deps, the loader is dlopen'd). New `prores_raw_videotoolbox`
hwaccel appears. The app builds in `build-ff9/` via the new macOS-only
cache var `-DQCV_FFMPEG_PREFIX=$PWD/external/install-ff9` (searched
before `external/install`; Helpers `ffmpeg`/`ffprobe` are staged from the
same prefix so CLI and dylibs never diverge), links `libav*.63` /
`libavutil.61`, `probe-video` passes on HEVC / ProRes / DNxHR /
DNxHR-MXF, `probe-metadata` output is identical to the 8.1.2 build, and a
35 s GUI launch on a ProRes 422 HQ clip opened cleanly (VideoToolbox
hwaccel attached, Metal zero-copy publish, audio decoder up, no
errors). Note `external/source/ffmpeg/` (the 8.1.2 tree) carries the
amended `dnxhddec.c` so `git apply --check -R` stays true; the installed
8.1.2 dylibs predate that one-line change, which is inert on 8.1.2.

```bash
# Trial build against 9.x without disturbing build/ or external/install/
cmake -S . -B build-ff9 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/Users/chris/Qt/6.11.1/macos \
  -DQCV_FFMPEG_PREFIX="$PWD/external/install-ff9"
cmake --build build-ff9 -j"$(sysctl -n hw.ncpu)"
```

**ffmpeg CLI (shipped Helpers / Windows `ffmpeg.exe`):** 9.0 removed
`-vsync`, `-top`, `-qphist`, `-filter_complex_script`,
`-adrift_threshold` and reworked `-re` / `-readrate`; TLS peer
verification is on by default. QCView itself never passes arguments
(it is only the provider via `toolbox.json`), but the qcbridge Blender
add-on spawns this binary — audit its command lines before shipping a
9.x CLI.

**Before adopting (in order) — all closed:**

1. ~~Re-verify the Avid ACT vector on the 9.0.1 build~~ — done 2026-09-08.
2. ~~macOS runtime pass on `build-ff9`~~ — chris, 2026-09-08 ("everything
   seems to be working").
3. ~~Windows: BtbN 9.0 rebuild + Vulkan/D3D11 runtime pass~~ — the 2.3.0
   Windows programme (23 commits, `git log 2608b554..066e4ffc`): the
   `lock_queue` question was settled by moving to
   `VK_KHR_internally_synchronized_queues` + app-owned cached Vulkan
   frame pools (the 2.2.8 device loss was our own pool churn); D3D11VA
   went zero-copy; FFV1/APV stay on the CPU (Vulkan 6–20× slower).
4. ~~Cut over~~ — macOS 2026-09-09: `external/install/` rebuilt from
   `n9.0.1` + 0001/0002/0003 with the unchanged recipe, 8.1.2 dylibs
   and source parked (see the macOS "Current build" bullet), the
   release build dir `build/` reconfigured against it (`QCV_FFMPEG_PREFIX`
   stays available for future side-by-side trials). macOS-specific
   follow-ups from the 2.3.0 Windows work (Metal RGBA16Unorm CPU slot
   for >8-bit sources, platform-neutral `firstSoftwareFormat`, dual
   RGBA64 un-gated) landed in the same pass.

### OCIO (OpenColorIO)

```cmake
FetchContent_Declare(
    ocio
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenColorIO.git
    GIT_TAG v2.5.0                              # latest 2.5.x stable
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v2.5.0** (bump to latest 2.5.x patch on schedule) |
| **License** | BSD-3-Clause |
| **Supported config profile** | Up to **2.5** (matches Guide 05 §12 / D17) |
| **Build flags** | `OCIO_BUILD_APPS=OFF`, `OCIO_BUILD_TESTS=OFF`, `OCIO_BUILD_PYTHON=OFF`, `OCIO_BUILD_GPU_TESTS=OFF` |
| **Required for** | Color management (Guide 05), OCIO snapshot in transcode (Guide 11) |

### OpenEXR

```cmake
FetchContent_Declare(
    openexr
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openexr.git
    GIT_TAG v3.4.7                              # current local Homebrew version
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v3.4.7** |
| **License** | BSD-3-Clause |
| **Verified** | Local install 3.4.7 confirmed |
| **Build flags** | `OPENEXR_INSTALL=OFF`, `OPENEXR_BUILD_TOOLS=OFF`, `BUILD_TESTING=OFF` |
| **Used by** | EXR sequence loader (Guide 02 §4), EXR layer detection (Guide 07 §5) |

### ink-stroke-modeler (Google)

```cmake
FetchContent_Declare(
    ink_stroke_modeler
    GIT_REPOSITORY https://github.com/google/ink-stroke-modeler.git
    # Pinned commit — current app tracks main, which is the bug we're fixing
    GIT_TAG <commit-hash-to-fill-on-first-build>
    GIT_SHALLOW FALSE                            # need full history for pin to resolve
)
```

| | Value |
|---|---|
| **Pin** | **commit hash, TBD at first build** |
| **License** | Apache 2.0 |
| **Verified** | Library is mature; specific commit verified at port time |
| **Caveat** | No regular release tags as of latest check. Pin to whatever HEAD is on day 1 of integration; document hash in this file's revision history. |
| **Build flags** | `INK_STROKE_MODELER_BUILD_TESTING=OFF`, `INK_STROKE_MODELER_FIND_DEPENDENCIES=OFF`, `INK_STROKE_MODELER_ENABLE_INSTALL=OFF` |
| **Used by** | Annotation stroke smoothing (Guide 04 §3) |

**Action item**: at first integration, fetch the ink-stroke-modeler
repo, pick `HEAD`, replace `<commit-hash-to-fill-on-first-build>`
with the actual hash, and update this entry.

### SoundTouch (Olli Parviainen)

```cmake
FetchContent_Declare(
    soundtouch
    GIT_REPOSITORY https://codeberg.org/soundtouch/soundtouch.git
    GIT_TAG 0047e0b1ecfceb041348579119bf79b73a322a3a   # tag 2.4.1
    GIT_SHALLOW FALSE
)
```

| | Value |
|---|---|
| **Pin** | **commit 0047e0b1 (= release tag 2.4.1, 2026 vintage)** |
| **License** | LGPL v2.1 |
| **License note** | Statically linked. LGPL-2.1 compliance is satisfied the same way the app's GPL FFmpeg build already forces: the whole work is distributed under GPL-compatible terms and source offers cover relinking. Notice in `LICENSES/SoundTouch-LICENSE.txt` + THIRD_PARTY_NOTICES §. |
| **Build flags** | `INTEGER_SAMPLES=OFF` (float pipeline), `SOUNDSTRETCH=OFF` (no CLI tool), `SOUNDTOUCH_DLL=OFF` |
| **Used by** | TempoStage (`src/audio/tempo_stage.{h,cpp}`) — constant-pitch WSOLA time-stretch for review-speed playback (0.5x–2x). Both decoder shapes route through it on their decode threads; 1x bypasses. |

### Abseil (transitive from ink-stroke-modeler)

```cmake
FetchContent_Declare(
    abseil
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG <matching-commit>                   # whatever ink-stroke-modeler uses
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **matched to ink-stroke-modeler's pinned Abseil commit** |
| **License** | Apache 2.0 |
| **Note** | ink-stroke-modeler usually fetches its own Abseil; we pin explicitly to avoid version drift. |
| **Build flags** | `ABSL_ENABLE_INSTALL=OFF`, `ABSL_PROPAGATE_CXX_STD=ON` |

### QtKeychain

```cmake
FetchContent_Declare(
    qtkeychain
    GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
    GIT_TAG v0.14.3                             # latest stable as of Q1 2026
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v0.14.3** |
| **License** | BSD-3-Clause |
| **Used by** | Frame.io API token storage (Guide 04 D8 / Guide 07) |
| **Build flags** | `BUILD_WITH_QT6=ON`, `BUILD_TEST_APPLICATION=OFF`, `BUILD_TRANSLATIONS=OFF` |
| **Platform hooks** | macOS Keychain Services, Windows Credential Manager, Linux libsecret / KDE KWallet — selected automatically based on platform |

### OpenTimelineIO (OTIO)

```cmake
FetchContent_Declare(
    opentimelineio
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenTimelineIO.git
    GIT_TAG v0.18.0                             # latest stable, bumped from old app's v0.17.0
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v0.18.0** |
| **License** | Apache 2.0 |
| **Used by** | Timeline data model across all source modes (Guide 02 §7), playlist serialization (Guide 09 §11) |
| **Build flags** | `OTIO_PYTHON_INSTALL=OFF`, `OTIO_INSTALL_PYTHON_MODULES=OFF`, `OTIO_FIND_IMATH=OFF` |
| **Note** | Bumped from old app's v0.17.0; verify schema compatibility with old `.qcvproj` files during migration testing |

### glslang (Khronos)

```cmake
FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG 14.3.0                              # tagged Khronos release
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **14.3.0** |
| **License** | BSD-3-Clause + Apache 2.0 (mixed) |
| **Used by** | Runtime SPIR-V compilation for OCIO-generated GLSL shaders on Vulkan backend (Guide 05 §11) |
| **Note** | Vulkan-side only; Metal backend skips this entirely (uses OCIO's MSL output directly). Build a stub on macOS to keep CMakeLists uniform. |

### nlohmann/json

```cmake
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3                             # current stable
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v3.11.3** |
| **License** | MIT |
| **Used by** | `.qcvproj` v2 schema (Guide 07 §9), shortcuts.json (Guide 02), color-presets.json (Guide 05), transcode-queue.json (Guide 11), every other JSON-backed settings file |
| **Build flags** | `JSON_BuildTests=OFF`, `JSON_Install=OFF` |
| **Alternative** | Qt 6 has built-in JSON support (`QJsonDocument`). Decision: use Qt's JSON for QML-bound model data; nlohmann for fixed-schema files where a real C++ type is preferable. |

---

## 3. System libraries

These are fixed minimum versions, resolved via platform package
managers on dev / CI; vendored at packaging time for distributable
binaries.

### Image format libraries

| Lib | Min version | Local | License | Used by |
|---|---|---|---|---|
| **libpng** | 1.6.0+ | 1.6.55 ✓ | libpng license (BSD-like) | PNG sequence loader (Guide 02 §4), screenshot encoder fallback if Qt's PNG path doesn't fit |
| **libjpeg-turbo** | 3.0.0+ | 3.1.3 ✓ | BSD-3-Clause | JPEG sequence loader |
| **libtiff** | 4.5.0+ | 4.7.1_1 ✓ | libtiff license (BSD-like) | TIFF sequence loader |

Not vendored as FetchContent because:
- Universally available via system packages.
- Qt 6 already links these for `QImage`.
- ABI is stable across patch versions.

### Vulkan SDK (Win + Linux only)

| Component | Pin | Notes |
|---|---|---|
| **Vulkan SDK** | 1.3.280+ | LunarG distribution; required for Vulkan headers + validation layers in dev builds |
| **VK_KHR_video_decode_*** extensions | required at runtime | mature on NVIDIA + AMD + recent Intel as of early 2026 |

macOS uses Metal directly; no Vulkan SDK needed.

### Audio output

| Platform | API | Notes |
|---|---|---|
| **macOS** | CoreAudio (via `QAudioSink`) | system framework; Qt wraps |
| **Windows** | WASAPI (via `QAudioSink`) | system; Qt wraps |
| **Linux** | PipeWire / PulseAudio (via `QAudioSink`) | Qt selects automatically |

No additional pinning needed — Qt's `QAudioSink` is the abstraction.

---

## 4. Bundled assets (in repo)

### OCIO configs (`assets/OCIO/`)

| Config | Source | License | Notes |
|---|---|---|---|
| **Blender 5.2** | Blender repo (release v5.2) | CC0 | Default config (Guide 05 §5). Linear sRGB EDR / Linear P3 EDR displays for macOS HDR. Adds Apple Log 2 input + texture-encoding spaces over 5.1. |
| **Blender 5.1** | Blender repo (release v5.1) | CC0 | Kept alongside 5.2 — user presets store the config name with no migration path. Same EDR display patch. |
| **Blender 4.5** | Blender repo (release v4.5) | CC0 | Fallback config |
| **ACES 2.0** | OCIO Configs project (`studio-config-all-views-v4.0.0_aces-v2.0_ocio-v2.5`) | CC0 | **Will be patched at port time** to add EDR display entries (Guide 05 D12) |
| **ACES 1.3** | OCIO Configs project (`studio-config-v1.0.0_aces-v1.3_ocio-v2.1`) | CC0 | Legacy compatibility |

### Safety guide SVGs (`assets/safety/`)

12 SVG files ported verbatim from the current app (Guide 10 §3):
`16x9.svg`, `Youtube_16x9_Masthead.svg`, `Youtube_16x9.svg`,
`TikTok_9x16.svg`, `Youtube_9x16.svg`, `Meta_Reels_9x16.svg`,
`Meta_Stories_9x16.svg`, `Pinterest_9x16.svg`,
`Samsung_9x16_Safety.svg`, `Snapchat_9x16_unofficial.svg`,
`Youtube_1x1.svg`, `Pinterest_PremiumSpotlight_1x1.svg`.

Plus a `custom/` subdirectory for user-imported SVGs (Guide 10 D6).

### Exiftool binary (`assets/exiftool/`)

Bundled as in current app for Adobe XMP metadata extraction
(Guide 07 §6). Per-platform binaries:

| Platform | Source | License |
|---|---|---|
| macOS | exiftool.org/macOS_distribution | Artistic 2.0 / GPL (dual) |
| Windows | exiftool.org/Windows_distribution | same |
| Linux | bundled or system-`exiftool` | same |

Pin the version once per shipped binary; update annually.

### Fonts (`assets/fonts/`)

| Font | License | Used for |
|---|---|---|
| **Inter** | OFL | UI text, PDF export body |
| **JetBrains Mono** | OFL | code blocks in markdown, monospace timecode |
| **Material Symbols** (icon font) | Apache 2.0 | icons throughout the UI |

Stable; no version pin needed beyond the asset files themselves.

---

## 5. What we explicitly DON'T depend on

Sometimes documenting absence matters as much as documenting presence.

| Not a dep | Why not | Replacement |
|---|---|---|
| **libharu** | Used by current app for PDF export | `QPdfWriter` + `QTextDocument` (Guide 04 §10) |
| **md4c / cmark-gfm** | Custom markdown parsers | Qt 6 built-in `MarkdownDialectGitHub` (Guide 04 D11) |
| **stb_image_write** | PNG encoding | `QImage::save()` (Guide 04 §8) |
| **NanoVG** | Per-platform annotation rendering | Unified QRhi stroke pass (Guide 04 D5) |
| **GLFW** | Window management | Qt's `QQuickWindow` |
| **GLEW / GLAD** | OpenGL loader | We don't use OpenGL (Guide 01 D4) |
| **D3D11 SDK** | Direct3D 11 | Vulkan via QRhi (Guide 01 D3) |
| **D3D12 SDK** | Direct3D 12 | Same |
| **MoltenVK** | Vulkan-on-Metal translation | Native Metal via QRhi (Guide 01 D3) |
| **imnodes** | Node graph editor | Slot-machine layout (Guide 05 D5) |
| **NativeFileDialog (NFD)** | Native file dialogs | Qt's `QFileDialog` |
| **stb_image** | Image decode | Qt + libpng/libjpeg/libtiff |
| **ImGui + ImPlot** | UI framework | Qt Quick |

---

## 6. Bump procedure

When bumping any dep:

1. Pick the new tag/commit.
2. Update this file's pin entry. Bump the date in §0 below.
3. Update `external/CMakeLists.txt` to match.
4. Run a full CMake configure + clean build on macOS, Windows, Linux.
5. Run the test matrix (TBD — at minimum: open old `.qcvproj`, transcode
   one ProRes clip, draw an annotation, save and reload).
6. Append to `dependencies-changelog.md`:
   - Date
   - Dep name + old pin → new pin
   - Reason for bump
   - Tested platforms
   - Anything to watch for in production

If the bump touches OCIO's profile version, update Guide 05 §12's
"supported config profile" entry too.

---

## 7. Quick-reference summary table

| Dep | Pin | Source | Category |
|---|---|---|---|
| Qt | 6.11.0 | installer | SDK |
| FFmpeg | n9.0.1 (macOS self-built; Win self-built BtbN recipe, 9.0 branch) — both + 3 local patches | self-built on both, vendored in-tree | core |
| OCIO | v2.5.0 | FetchContent | core |
| OpenEXR | v3.4.7 | FetchContent | core |
| ink-stroke-modeler | (commit TBD) | FetchContent | core |
| Abseil | (matched) | FetchContent | core (transitive) |
| QtKeychain | v0.14.3 | FetchContent | secure storage |
| OpenTimelineIO | v0.18.0 | FetchContent | timeline |
| glslang | 14.3.0 | FetchContent | shader compile |
| nlohmann/json | v3.11.3 | FetchContent | JSON |
| libpng | 1.6.0+ | system | image |
| libjpeg-turbo | 3.0.0+ | system | image |
| libtiff | 4.5.0+ | system | image |
| Vulkan SDK | 1.3.280+ | platform | Win+Linux only |

---

## 0. Revision history

| Date | Change | By |
|---|---|---|
| 2026-04-24 | Initial pin manifest from port plan | Plan + Week-0 verification |
| 2026-06-24 | Windows FFmpeg: BtbN GPL-Shared `n8.1-11-g75d37c499d` → `n8.1.2-20260624` (vendored in-tree at `external/ffmpeg-win64/`, gitignored). Security fix for CVE-2026-8461 "PixelSmash". See `dependencies-changelog.md`. | Chris |
| 2026-06-24 | macOS FFmpeg: self-built `n8.1` → `n8.1.2` (rebuilt in-tree at `external/install/`, gitignored). Same CVE-2026-8461 fix; sonames unchanged (62/60/62), ABI-clean drop-in. Also dropped vestigial `--enable-nonfree`. See `dependencies-changelog.md`. | Chris |
| 2026-08-20 | Windows FFmpeg: BtbN prebuilt `n8.1.2-20260624` → **self-built BtbN-recipe** `n8.1.2-44-g7c533d0f86-20260820` (WSL2+Docker, same toolchain image/flags/DLL majors) so Windows carries the two local patches (`external/patches/ffmpeg/`: DNxHR 444 ACT + untagged-limited convention, MXF RGBA range). Patches must be re-applied on every refresh — recipe in §Windows above. See `dependencies-changelog.md`. | Chris |
| 2026-09-08 | FFmpeg 9.0 investigated on branch `ffmpeg-9` (NOT adopted; pin stays `n8.1.2`): all library majors bump (61/63/63/63/12/10/7); app needed one source change (`av_opt_set_int_list` → `av_opt_set_array`, dual-version safe) + DLL names derived from pkg-config; patch 0001 regenerated to sync `sw_pix_fmt` (9.0 probe-label interaction); `n9.0.1` built into `external/install-ff9/` and the app built/linked via new `QCV_FFMPEG_PREFIX`. Details + remaining steps in §2 "FFmpeg 9.0 — migration status". | Claude |
| 2026-09-09 | **FFmpeg `n8.1.2` → `n9.0.1` on both platforms** (Windows 2.3.0 shipped 2026-09-08 on the BtbN-recipe 9.0.1 build with patches 0001–0003; macOS `external/install/` rebuilt 2026-09-09, 8.1.2 parked). All sonames/DLL majors change (63/61/63/12/63/7/10). New patch 0003 (ProRes RAW Bayer patterns). See §2 "FFmpeg 9.0 — migration status" and `dependencies-changelog.md`. | Chris (Win) / Claude (mac) |

(Append future bumps here.)
