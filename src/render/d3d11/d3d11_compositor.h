// D3D11Compositor — Phase F.2.3.
//
// Single-pass D3D11 pixel-shader that draws a video / image source into
// a destination RTV with aspect-preserving fit (letterbox / pillarbox).
// Background fill where the source rect doesn't cover the destination.
//
// F.2.3 scope: Single source, RGBA8 input, Black background only.
// SideBySide / Wipe modes, the other background fills (DarkGray, light
// + dark checkerboards), and per-side activity flags land in F.2.3+
// follow-ons before the OCIO pass (F.2.5).
//
// Mirrors MetalCompositor's public API loosely — the C++ side is
// platform-specific (D3D11 PSO state objects + HLSL shaders embedded
// as strings), but the entry point shape (renderSingle with src view +
// dest extent + source extent) is the same so the renderer's per-frame
// invocation matches macOS.

#pragma once

#include <memory>

namespace qcv {

class D3D11Compositor {
public:
    D3D11Compositor();
    ~D3D11Compositor();

    D3D11Compositor(const D3D11Compositor &)            = delete;
    D3D11Compositor &operator=(const D3D11Compositor &) = delete;

    // Compile shaders + build pipeline state. Requires
    // D3D11DeviceManager to be initialized.
    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Draw one fullscreen-triangle pass into the bound RTV.
    // `ctx`     — immediate context (caller has already bound RTV +
    //              set viewport).
    // `srcSrv`  — shader resource view of the source texture; may be
    //              nullptr (renders background only).
    // `dstW/H`  — destination viewport extent in pixels.
    // `srcW/H`  — source content size (for aspect-fit math); ignored
    //              when srcSrv is null.
    // `bgMode`  — qcv::BackgroundMode enum:
    //              0 = Black, 1 = DarkGray,
    //              2 = DarkCheckerboard, 3 = LightCheckerboard.
    // `borderPx`        — edge-frame width in px; 0 (default) = no
    //                      frame. Used by renderCornerOverlay so the
    //                      thumbnail doesn't blend into the viewport.
    // `borderR/G/B`     — frame color (only when borderPx > 0).
    // `overlayBlend` — when true, the source is blended STRAIGHT over the
    //                  existing RTV contents (non-premult src-over) and
    //                  fragments outside the source rect are transparent,
    //                  instead of compositing over an opaque bg fill. Used
    //                  for the centered viewport-notice card so its rounded
    //                  corners reveal the viewport background (parity with
    //                  the macOS present compositor). Default false.
    // `rotQuarters` — display rotation in quarter-turns CW {0..3}. The
    //                  caller passes srcW/H already SWAPPED for odd
    //                  quarters (display-orientation dims drive the fit);
    //                  the shader inverse-rotates its sampling so the
    //                  stored texture reads upright.
    // `opts`        — present-pass options (media-bounds fill, output
    //                  encoding, source-only pass); see PassOptions.
    //                  nullptr = plain fill, SDR encoding, prior behavior.

    // Media-bounds-aware background (mirror of
    // MetalCompositor::BackgroundLayout). Describes where the media
    // sits so the fill OUTSIDE the media rect(s) is blended toward
    // Theme.bg (#161616), making the bounds of fully / partly
    // transparent media visible. Black mode opts out (solid black). Geometry chain: `canvasW/H` is the
    // texture being presented (aspect-fit 1:1 into the destination),
    // then each source is aspect-fit into its region of that canvas
    // exactly like D3D11DualCompositor's sampleFit does.
    //
    //   layoutMode: 0 Single (A full canvas), 1 SideBySide (A left
    //               half / B right half), 2 Wipe (A where canvas
    //               u < splitPos, else B), 3 Difference (union).
    //   srcA/B:     display-orientation effective dims (PAR
    //               un-squeezed, rotation-swapped).
    //   aValid/bValid: side has (or had) a source; false → no rect.
    //               Past-end sides stay valid so the footprint remains.
    struct BackgroundLayout {
        int   canvasW = 0, canvasH = 0;
        int   layoutMode = 0;
        float splitPos = 0.5f;
        int   srcAW = 0, srcAH = 0;
        int   srcBW = 0, srcBH = 0;
        bool  aValid = false;
        bool  bValid = false;
    };

    // How the fill constants (sRGB-encoded) must be written for the
    // bound swapchain format. Same three variants — and the same
    // reference luminance — as D3D11AnnotationRenderer (Phase F.2.9):
    //   Sdr         B8G8R8A8_UNORM / G22       → sRGB values verbatim
    //   ScRgbLinear R16G16B16A16_FLOAT / G10   → srgbToLinear · refNits/80
    //   Hdr10Pq     R10G10B10A2_UNORM / G2084  → PQ(709→2020 · refNits/1e4)
    enum class OutputEncoding : int {
        Sdr         = 0,
        ScRgbLinear = 1,
        Hdr10Pq     = 2,
    };
    // Pick the variant from a DXGI_FORMAT (passed as int so this
    // header stays free of <dxgiformat.h>).
    static OutputEncoding encodingForFormat(int dxgiFormat);
    // Reference luminance for the scRGB / PQ fill encodes (nits).
    // Default 200 (Guide 06 D11); SDR ignores it.
    void setReferenceLuminance(float nits);

    struct PassOptions {
        // Source-only pass: write the source STRAIGHT (rgb, a) inside
        // its fit rect and (0,0,0,0) outside — no fill, opaque blend
        // state. Used to render the picture into the OCIO intermediate
        // so the real background can be drawn post-OCIO on the
        // swapchain (Metal draws its fill in the present pass; the
        // fill must never go through the display transform).
        bool sourceOnly = false;
        // Multiply the source by the compositor's brightness. Off for
        // the single-flow present pass, where the source pass already
        // applied it pre-OCIO. The fill is never brightness-scaled.
        bool applyBrightness = true;
        // Media footprint for the bounds fill. nullptr → whole
        // destination is "inside" (plain fill).
        const BackgroundLayout *layout = nullptr;
        OutputEncoding encoding = OutputEncoding::Sdr;
    };

    void renderSingle(void *ctx,
                       void *srcSrv,
                       int   dstW, int dstH,
                       int   srcW, int srcH,
                       int   bgMode,
                       float borderPx = 0.0f,
                       float borderR = 0.0f,
                       float borderG = 0.0f,
                       float borderB = 0.0f,
                       bool  overlayBlend = false,
                       int   rotQuarters = 0,
                       const PassOptions *opts = nullptr);

    // Phase F.2.8 follow-up — uniform output multiplier applied to
    // the final pixel before write. 1.0 = identity. Stored as state;
    // next renderSingle picks it up.
    void setBrightness(float brightness);

    // Hover-thumb corner overlay. Mirrors MetalCompositor's
    // renderCornerOverlay (metal_compositor.mm:522). Reuses the
    // renderSingle pipeline by setting a tiny viewport at the chosen
    // corner and treating that rect as a fake canvas — the existing
    // aspect-fit shader letter-/pillarboxes the thumb inside the box
    // (bgMode 0 = black fill) and draws a faint edge frame. Box height
    // is capped at the box width, so a tall portrait thumb is never
    // taller than wide. Caller's viewport is saved + restored so
    // downstream draws (annotation, screenshot, present) aren't
    // affected.
    //
    //   ctx          — immediate context, RTV already bound by caller.
    //   srcSrv       — thumbnail SRV (from
    //                  D3D11TexturePool::thumbnailInstance().texture()).
    //   srcW/H       — thumbnail source dimensions for aspect ratio.
    //   dstW/H       — caller's full draw extent in pixels (the canvas
    //                  the corner anchors INTO; in dual flow pass the
    //                  canvas-fit rect, NOT the raw swapchain, so
    //                  overlays don't land in letterbox bars).
    //   corner       — 0 = bottom-left (side A), 1 = bottom-right (B).
    //   overlayFrac  — fraction of dstW the box width occupies;
    //                  clamped to [0.05, 0.5].
    //   marginPx     — inset from the canvas edges.
    //   rotQuarters  — display rotation for the thumb source, quarter-
    //                  turns CW; srcW/H arrive display-swapped like
    //                  renderSingle's.
    void renderCornerOverlay(void *ctx,
                              void *srcSrv,
                              int   srcW, int srcH,
                              int   dstW, int dstH,
                              int   corner,
                              float overlayFrac,
                              float marginPx,
                              int   rotQuarters = 0);

    // Loading spinner — full-viewport dark fill + rotating accent dots.
    // No source texture. `timeSeconds` drives the animation (render
    // thread supplies elapsed-since-load-start). Caller binds the RTV +
    // viewport first.
    void renderSpinner(void *ctx, int dstW, int dstH, float timeSeconds);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
