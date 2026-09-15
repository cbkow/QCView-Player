# Handoff → Windows session: media-bounds background fill + HDR-correct fill

macOS shipped this in a0c6725a (2026-09-15). This note is what the Windows
(D3D11) side needs to match it. Retire this file once the items are closed,
same as HANDOFF-macOS.md was (46980918).

## What macOS does now (the spec to match)

The viewport background pass knows where the media sits. Every drawable pixel
OUTSIDE the media rect(s) is filled with a theme grey; INSIDE keeps the
user's chosen background (black / dark grey / checker), so a fully or partly
transparent clip still shows its footprint.

| BackgroundMode  | inside the media rect | outside (sides / letterbox) |
|-----------------|-----------------------|-----------------------------|
| 0 Black         | #000000               | #161616 (Theme.bg)          |
| 1 DarkGray      | #161616               | #1f1f1f (Theme.toolbar)     |
| 2 DarkChecker   | 30/20 checker         | #161616                     |
| 3 LightChecker  | 200/178 checker       | #161616                     |

Rules, all signed off by chris — do not re-open them:
- Always on. No setting, no per-clip alpha gating.
- Hard edge, no feather. The edge must land on the SAME pixel as the
  compositor's letterbox edge (no 1 px seam on opaque content). On macOS the
  shader repeats the compositor's aspect-fit math in float instead of taking
  a CPU-rounded rect.
- Implemented as `mix(fill, target, strength)` with strength 1.0 for all four
  modes; keep the mix so `QCV_BOUNDS_MIX` (env, "0.7" or "1.0,0.5,1.0,0.7"
  = black,darkgray,darkChecker,lightChecker) still tunes it at launch.
- Dual view is per region: SideBySide = A's fit in the left half, B's in the
  right half. Wipe = A's full-canvas fit where canvas u < splitPos, B's fit
  elsewhere. Difference = union of both fits. A side that is past its clip
  end keeps its LAST footprint (its cache dims are zeroed at past-end, so
  stash dims while the side has a texture). A side that never had a source
  contributes no rect. No source at all → plain fill everywhere (old look).
- No-media / colour-bars / spinner / hover-thumbnail / notice-card paths are
  untouched. Screenshots are untouched (they render the source natively).

Reference implementation to read first:
- `src/render/metal/metal_compositor.h` — `MetalCompositor::BackgroundLayout`
  and the 7-arg `renderBackground` overload (doc comment explains the
  geometry chain).
- `src/render/metal/metal_compositor.mm` — `kBackgroundMSL` (`insideFit`,
  `insideMedia`, `srgbToLinear`, `bg_fs`) and, below it, `boundsStrengths()` /
  `boundsPolicyFor()` (targets + env override) and the UBO fill.
- `src/dual/metal/dual_compositor.{h,mm}` — `LastLayout` / `lastLayout()`,
  filled at the end of `renderFrame()`.
- `src/render/metal/metal_player_renderer.mm` — the two `renderBackground`
  call sites (single present pass, dual present pass) and `isLinearHdrMode`.

## D3D11 state today (why it isn't a straight port)

1. **Single flow draws the background PRE-OCIO.** `d3d11_player_renderer.cpp`
   ~1195: `compositor.renderSingle(videoA, …, m_bgMode)` into the intermediate
   RTV, then `ocio.apply(intermediate → swapchain)`. The fill therefore goes
   through the OCIO display transform as if it were scene-referred. Metal
   draws the fill post-OCIO in the present pass, and so does D3D11's own
   dual flow (~1598: `renderSingle(correctedCanvas, …, m_bgMode)` onto the
   swapchain). Fix the divergence first: in the OCIO branch of the single
   flow, render the source with bgMode = Black/transparent into the
   intermediate and draw the real background post-OCIO, or restructure the
   present so the fill is applied on the swapchain like the dual path. The
   bypass branch (~1225, no OCIO) can keep drawing straight to the swapchain.
2. **Single compositor already has the fit rect.** `d3d11_compositor.cpp`
   `PSMain` gets `fitRectMin/fitRectSize` in its cbuffer and does
   `backgroundColor(fragPx)` for pixels outside — so for single flow the rule
   is: inside fitRect → fill; outside → `lerp(fill, target, strength)`.
   Because fill and source share one shader here, there is no seam risk.
3. **Dual present has no rects.** The dual present blits the canvas through
   the same `renderSingle` with the canvas as source, so `fitRect` is the
   canvas rect, not the media. `backgroundColor()` needs a second input set:
   canvas dims + srcA/srcB effective dims + mode + splitPos + aValid/bValid,
   and must repeat `D3D11DualCompositor`'s `sampleFit` math (kPsHlsl in
   `d3d11_dual_compositor.cpp`, a verbatim port of the Metal one). Expose the
   per-side effective dims from `D3D11DualCompositor` the way Metal's
   `lastLayout()` does — it already computes them (PAR un-squeeze, rotation
   swap) when filling its constant buffer.
4. **Constant buffer.** `CompositorCB` in `d3d11_compositor.cpp` is a
   64-byte struct with `static_assert`s on size and 16-byte packing. Adding
   canvasSize, srcSizeA, srcSizeB (float2 each), target (float4), layoutMode,
   splitPos, aValid, bValid, hasLayout, linearOutput/encode needs new 16-byte
   rows; update the HLSL cbuffer AND the assert together.

## HDR-correct fill (second half of the change)

The fill constants are sRGB-encoded. Metal decodes them (sRGB EOTF) when the
drawable is linear-light so DarkGray matches the #161616 rails again. On
Windows the swapchain formats are (`d3d11_hdr_swapchain.cpp` `mappingFor`):

| HdrMode                 | format / colour space                 | what the fill must write                                   |
|-------------------------|---------------------------------------|------------------------------------------------------------|
| SdrSRgb, SdrDisplayP3   | B8G8R8A8_UNORM, G22_NONE_P709         | sRGB values as today                                       |
| ExtendedLinearSRgb / P3 | R16G16B16A16_FLOAT, G10_NONE_P709     | `srgbToLinear(c) * refNits / 80` (scRGB: 1.0 = 80 nits)     |
| Hdr10                   | R10G10B10A2_UNORM, G2084_NONE_P2020   | `PQ(BT709→BT2020(srgbToLinear(c)) * refNits / 10000)`       |

There is already a verified implementation of exactly these three variants
in `d3d11_annotation_renderer.cpp` (Phase F.2.9: `srgbToLinear`, the scRGB
scale, the PQ encode, variant picked from the swapchain DXGI format, and
`setReferenceLuminance(200.0f)` at `d3d11_player_renderer.cpp` ~418). Reuse
that: same helpers, same `refNits` (200 nits, Guide 06 D11 default), same
format-keyed variant selection. Pass the swapchain format (or a variant enum)
into `D3D11Compositor` so `backgroundColor()` encodes its output the same way.

Note the interaction with item 1 above: if the fill stays pre-OCIO in the
single flow, OCIO would encode it (wrongly, as scene-referred). Post-OCIO is
the only place where the explicit encode is correct — which is another
reason to move it.

Expected checks: `#161616` viewport tone == the rail panels in scRGB and
HDR10 modes (Windows composes the Qt chrome at SDR-white level, so the FP16
swapchain has to be scaled by refNits/80 to land on the same grey);
Black stays black; checkers look the same as SDR.

## Verification recipe (mirror of what was run on macOS)

Generate alpha clips with the vendored FFmpeg (note: `drawbox` on an rgba
source does NOT write alpha — the clip ends up fully transparent; use
`overlay`):

```
ffmpeg -f lavfi -i "color=c=black@0.0:s=1920x1080:d=4:r=24,format=rgba" \
       -f lavfi -i "color=c=orange@0.5:s=600x400:d=4:r=24,format=rgba" \
       -filter_complex "[0][1]overlay=400:300:format=auto,format=rgba" \
       -c:v prores_ks -profile:v 4444 -pix_fmt yuva444p10le alpha_16x9.mov
```
Make a 1440x1080 and a 1080x1920 variant, plus an opaque `testsrc2` clip for
the seam check. Then, per `feedback_verify_each_media_path`:
- `--playlist-test DIR` (all four clips): each background mode; edge scan on
  the opaque clip shows fill → picture with no intermediate pixel.
- `--simulate-user A B --sbs`: both footprints marked; then Wipe and
  Difference from the toolbar; let one side run past its end.
- `--hdr-mode 2` (scRGB) and `--hdr-mode 4` (HDR10): DarkGray viewport ==
  rail tone; Black still black. (`--hdr-mode` is new in a0c6725a, in
  `main.cpp` next to the other dev flags; the HDR mode is not persisted.)
- Hover thumbnails, notice card, screenshots unchanged.
- Background mode can be preset in the registry/QSettings key
  `display/backgroundMode` (0..3) instead of clicking the LeftRail swatches.

## Known gaps to leave alone (recorded, not in scope)

- Checker tile size: Metal 20 pt × contentsScale, D3D11 fixed 32 px.
- `computeDualViewLayout` in `src/render/dual_view_layout.cpp` has no callers.
- Metal annotation renderer writes sRGB stroke/safety colours into linear
  EDR drawables (D3D11 already handles this via F.2.9; macOS does not yet).
