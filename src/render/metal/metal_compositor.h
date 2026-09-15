// MetalCompositor — Phase 7.5 B.6.1 / B.6.7 / Guide 19.
//
// Single fragment shader handles three modes via uniform:
//   Single     — source aspect-fit into the drawable, letterbox/
//                pillarbox in caller's clear color.
//   SideBySide — source rendered twice, once in each half (full
//                width on each side, aspect-fit). Per memory note
//                Phase 2.4: dual-view was dropped as a media-item
//                type, so we have ONE source — both sides show the
//                same content. Visual cue is the split, not actual
//                A/B compare.
//   SplitWipe  — source aspect-fit into drawable; vertical splitPos
//                separator overlaid as a 1px white line.
//
// Adapted from old QCView's src/gpu/metal_dual_compositor.{h,mm}:
// the old compositor wrote into a separate RGBA16F output texture
// for the OCIO chain to sample. The new flow draws straight into
// the swapchain's render pass — there's no OCIO between the
// compositor and the drawable yet (Phase B.6.4 inserts that and
// reintroduces an intermediate texture).
//
// Threading: created + used on the render thread. Pipeline state
// caching is local; no cross-thread state.

#pragma once

namespace qcv {

class MetalCompositor {
public:
    MetalCompositor();
    ~MetalCompositor();

    MetalCompositor(const MetalCompositor &)            = delete;
    MetalCompositor &operator=(const MetalCompositor &) = delete;

    // `targetPixelFormat` is the drawable's MTLPixelFormat passed as
    // an int (the value from layer.pixelFormat). The pipeline state
    // is per-format; if the format changes (HDR toggle), call
    // shutdown() + initialize() again.
    //
    // `enableSrcOverBlending`: when true, the pipeline blends the
    // shader's output over the destination using standard premul-
    // less source-over (src=SrcAlpha, dst=OneMinusSrcAlpha for RGB;
    // src=One, dst=OneMinusSrcAlpha for alpha). Use this for the
    // present pass that draws the source on top of the bg fill so
    // ProRes 4444 alpha video composites over the bg correctly and
    // the swapchain ends up fully opaque (otherwise the swapchain
    // inherits the source's <1 alpha and macOS alpha-blends the
    // window over the desktop, producing fringey edges).
    //
    // Canvas / capture compositors should keep this OFF — they
    // write into intermediates whose alpha is needed downstream
    // (compositeRaw → screenshot path).
    bool initialize(int targetPixelFormat,
                    bool enableSrcOverBlending = false);
    void shutdown();
    bool isInitialized() const;
    int  targetPixelFormat() const;

    // Mode enum mirrors qcv::CompositorMode (kept in sync at the
    // seam — same int values). Renderer passes through.
    enum Mode : int {
        Single     = 0,
        SideBySide = 1,
        SplitWipe  = 2,
    };

    // Uniform post-OCIO brightness multiplier — actually applied in
    // the compositor's fragment shader (pre-OCIO, linear-light), to
    // match D3D11's Phase F.2.9 placement. 1.0 = identity. Stored
    // on the compositor; baked into the UBO on every renderSources /
    // renderSource / renderSingle call.
    void setBrightness(float brightness);

    // Two-source render. `encoder` is an active
    // id<MTLRenderCommandEncoder> (passed as void* to keep this
    // header ObjC-clean). `srcATexture` / `srcBTexture` are
    // id<MTLTexture> — caller passes the same texture for both in
    // solo cases so SBS / Wipe still work as a no-op-compare.
    // Either may be nil; nil sides render as transparent (the
    // compositor discards those fragments).
    //
    // `aActive` / `bActive` distinguish "side intentionally inactive
    // (past its clip's end in dual mode)" from "side missing texture
    // but still expected." When a side is intentionally inactive we
    // skip the mirror-into-other-slot fallback so its region stays
    // transparent rather than showing the other source. Defaults
    // true so single-source callers don't notice.
    // `rotA` / `rotB` — per-side display rotation in quarter-turns CW
    // {0..3}. Callers pass src dims already SWAPPED for odd quarters
    // (display-orientation dims drive the fit); the MSL inverse-
    // rotates its sampling so the stored texture reads upright.
    void renderSources(void *encoder,
                       void *srcATexture, int srcAWidth, int srcAHeight,
                       void *srcBTexture, int srcBWidth, int srcBHeight,
                       int dstWidth, int dstHeight,
                       int mode = Single,
                       float splitPos = 0.5f,
                       bool aActive = true,
                       bool bActive = true,
                       int rotA = 0,
                       int rotB = 0);

    // Convenience for solo / final-blit callers — passes the same
    // texture as both A and B. Mode defaults to Single but the
    // caller can pass SBS / Wipe to drive the no-op-compare path.
    void renderSource(void *encoder, void *sourceTexture,
                      int srcWidth, int srcHeight,
                      int dstWidth, int dstHeight,
                      int mode = Single,
                      float splitPos = 0.5f,
                      int rot = 0)
    {
        renderSources(encoder, sourceTexture, srcWidth, srcHeight,
                      sourceTexture, srcWidth, srcHeight,
                      dstWidth, dstHeight, mode, splitPos,
                      true, true, rot, rot);
    }
    void renderSingle(void *encoder, void *sourceTexture,
                      int srcWidth, int srcHeight,
                      int dstWidth, int dstHeight)
    {
        renderSources(encoder, sourceTexture, srcWidth, srcHeight,
                      sourceTexture, srcWidth, srcHeight,
                      dstWidth, dstHeight, Single, 0.5f);
    }

    // SMPTE 75% color bars placeholder. Used by the player when no
    // source texture is available (cache warming up, no media loaded,
    // etc.). Self-contained — no input texture, no UBO; the shader
    // derives bar colors from the fragment UV. Pipeline state is
    // built lazily on first call and re-baked when the target pixel
    // format changes.
    void renderColorBars(void *encoder, int dstWidth, int dstHeight);

    // Phase 3.H.5 — corner overlay (hover-thumbnail). Samples
    // `thumbTexture` and aspect-fits it into a corner ROI of the
    // current encoder target. Designed to be called AFTER the main
    // renderSources pass but BEFORE OCIO so the thumb gets the
    // same color treatment as the main image. `corner`: 0=BL, 1=BR.
    // `overlayFrac` controls the corner box's width relative to dst
    // (height follows aspect). `marginPx` is the inset from each
    // edge. No-op if thumbTexture is null.
    // `rotQuarters` — display rotation for the thumb, quarter-turns
    // CW; thumbW/H arrive display-swapped like renderSources' dims.
    void renderCornerOverlay(void *encoder,
                              void *thumbTexture,
                              int   thumbW, int thumbH,
                              int   dstWidth, int dstHeight,
                              int   corner       = 0,
                              float overlayFrac  = 0.18f,
                              float marginPx     = 12.0f,
                              int   rotQuarters  = 0);

    // Viewport background fill — old QCView's DrawVideoBackground.
    // Drawn before the source pass so letterbox/pillarbox area
    // shows the chosen background. Modes:
    //   0 Black            — solid (0,0,0)
    //   1 DarkGray         — #1f1f1f (Theme.toolbar) inside the media,
    //                        #161616 outside; old app DEFAULT
    //   2 DarkCheckerboard — #2e2e2e / #1f1f1f tiles (matches D3D11)
    //   3 LightCheckerboard— #cccccc / #b3b3b3 tiles (matches D3D11)
    // tilePixels controls the checker tile size at 1:1 device-pixel
    // mapping (caller passes layer.contentsScale-aware value).
    void renderBackground(void *encoder, int mode,
                          int dstWidth, int dstHeight, float tilePixels);

    // Media-bounds-aware background. Describes where the media sits
    // so the fill can be blended toward a per-mode target color
    // OUTSIDE the media rect(s), making the bounds of fully/partly
    // transparent media visible. Mirrors the present-blit geometry:
    // `canvasW/H` (the compositeRaw intermediate) is aspect-fit 1:1
    // into the drawable, then each source is aspect-fit into its
    // region of that canvas exactly like sampleFit does — the shader
    // repeats that math in float so the edge lands on the same pixel
    // as the compositor's discard edge.
    //
    //   layoutMode: 0 Single (A full canvas), 1 SideBySide (A left
    //               half / B right half), 2 SplitWipe (A where
    //               canvas u < splitPos, else B), 3 Difference
    //               (union of A and B fits).
    //   srcA/B:     display-orientation effective dims (PAR
    //               un-squeezed, rotation-swapped) — the same values
    //               the compositor UBO gets.
    //   aValid/bValid: side has (or had) a source; false → no rect
    //               for that side. Past-end sides stay valid so the
    //               clip's footprint remains marked.
    struct BackgroundLayout {
        int   canvasW = 0, canvasH = 0;
        int   layoutMode = 0;
        float splitPos = 0.5f;
        int   srcAW = 0, srcAH = 0;
        int   srcBW = 0, srcBH = 0;
        bool  aValid = false;
        bool  bValid = false;
    };

    // `layout` may be null → whole viewport is treated as "inside"
    // (plain fill, prior behavior). `linearOutput` = the drawable is
    // linear-light with 1.0 = SDR white (EDR / extended-linear
    // modes): the sRGB-space fill constants are EOTF-decoded before
    // being written so the viewport tone matches the Qt chrome.
    void renderBackground(void *encoder, int mode,
                          int dstWidth, int dstHeight, float tilePixels,
                          const BackgroundLayout *layout,
                          bool linearOutput);

private:
    struct Impl;
    Impl *m_impl = nullptr;
};

} // namespace qcv
