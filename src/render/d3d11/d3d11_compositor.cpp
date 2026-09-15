#include "d3d11_compositor.h"
#include "d3d11_device_manager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <QByteArray>
#include <QList>
#include <QtLogging>

#include <algorithm>
#include <array>
#include <cstring>

namespace qcv {

using Microsoft::WRL::ComPtr;

namespace {

// Vertex: fullscreen triangle with Y-flip for D3D11 top-left screen
// origin. Same shape as the F.1.a/F.1.b probe vertex shader (also
// used elsewhere in this codebase).
constexpr const char *kVsHlsl = R"(
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VsOut VSMain(uint vid : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
)";

// Pixel: aspect-fit single source on top of a background fill (solid
// black / solid dark grey / 32 px checkerboard in two greys). hasSrc=0
// paints background only; hasSrc=1 samples src inside fitRect and
// falls back to background outside. bgMode picks which fill.
//   0 = Black                  (0.000, 0.000, 0.000) — solid everywhere,
//                               no bounds blend (some viewers want
//                               plain black; chris 2026-09-15)
//   1 = DarkGray   ≈ 31/255    (0.1216 — #1f1f1f, Theme.toolbar inside
//                               the media; #161616 Theme.bg outside)
//   2 = DarkCheckerboard       (#1f1f1f / #2e2e2e)
//   3 = LightCheckerboard      (#b3b3b3 / #cccccc)
//
// Media-bounds fill (2026-09-15, ported from kBackgroundMSL on macOS):
// with hasLayout=1 every pixel OUTSIDE the media rect(s) blends the
// fill toward `target.rgb` (#161616 Theme.bg) by `target.a`, so a
// fully / partly transparent clip still shows its footprint. Single layouts get the
// CPU-computed `mediaRect` (the same fit + comparison the source pass
// uses, so the edge lands on the same pixel); dual layouts repeat
// D3D11DualCompositor's sampleFit math in float.
//
// The fill constants are sRGB-encoded. `encoding` re-encodes them for
// the bound swapchain (0 SDR verbatim, 1 scRGB linear, 2 HDR10 PQ) —
// the same helpers and reference luminance as the annotation renderer
// (Phase F.2.9). The source is never re-encoded here: OCIO (or the
// bypass) already produced swapchain-space values.
constexpr const char *kPsHlsl = R"(
cbuffer Constants : register(b0) {
    float2 dstSize;        // destination viewport, pixels
    float2 fitRectMin;     // source-fit top-left, pixels
    float2 fitRectSize;    // source-fit size, pixels
    float  borderPx;       // edge-frame width in px; 0 = no border
    float  _pad0;
    int    hasSrc;
    int    bgMode;
    int    overlayMode;    // 0 = composite over bg fill (opaque output);
                           // 1 = straight src for hardware src-over blend,
                           //     transparent outside the source rect (the
                           //     viewport notice card; mirrors Metal's
                           //     present-compositor src-over + discard).
                           // 2 = source-only: straight src inside the fit
                           //     rect, (0,0,0,0) outside, opaque blend —
                           //     the OCIO intermediate pass.
    int    rotQ;           // display rotation, quarter-turns CW {0..3}
    float  brightness;     // source multiplier; 1.0 = identity
    float3 borderColor;    // RGB of the edge frame (used when borderPx > 0)
    // ---- media-bounds fill + output encoding (row 4 onward) ----
    float2 canvasSize;     // presented texture dims (aspect-fit into dst)
    float2 srcSizeA;       // display-orientation effective dims
    float2 srcSizeB;
    int    layoutMode;     // 0 single, 1 sbs, 2 wipe, 3 difference
    float  splitPos;
    int    aValid;
    int    bValid;
    int    hasLayout;      // 0 = no media rect info → everything "inside"
    int    encoding;       // 0 SDR, 1 scRGB linear, 2 HDR10 PQ
    float4 target;         // rgb = outside-target color (sRGB), a = strength
    float4 mediaRect;      // layoutMode 0: xy = min, zw = size (dst pixels)
    float  refNits;        // reference luminance for encodings 1 / 2
    int    applyBrightness;// 0 = source written without the multiplier
    float2 _pad1;
};

Texture2D    src       : register(t0);
SamplerState srcSamp   : register(s0);

struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Display→stored UV for the quarter-turn rotation. `p` is the
// normalized position inside the (display-orientation) fit rect;
// the fit was computed from swapped dims for odd quarters, so the
// inverse rotation here lands back on the stored texture upright.
float2 rotatedSrcUv(float2 p)
{
    if (rotQ == 1) return float2(p.y, 1.0 - p.x);          //  90 CW
    if (rotQ == 2) return float2(1.0 - p.x, 1.0 - p.y);    // 180
    if (rotQ == 3) return float2(1.0 - p.y, p.x);          // 270 CW
    return p;
}

float4 backgroundColor(float2 fragPx)
{
    if (bgMode == 1) {
        // 31/255 = 0.1216 — #1f1f1f, Theme.toolbar. Chris flipped the
        // greys 2026-09-15: the media footprint takes the chrome's alt
        // grey and the outside falls to Theme.bg like every other mode.
        return float4(0.1216, 0.1216, 0.1216, 1.0);
    } else if (bgMode == 2) {
        // Dark checkerboard — 32 px squares.
        int2 cell = int2(fragPx) / 32;
        bool odd = ((cell.x + cell.y) & 1) != 0;
        float v = odd ? 0.180 : 0.122;
        return float4(v, v, v, 1.0);
    } else if (bgMode == 3) {
        // Light checkerboard — same 32 px grid, brighter pair.
        int2 cell = int2(fragPx) / 32;
        bool odd = ((cell.x + cell.y) & 1) != 0;
        float v = odd ? 0.800 : 0.702;
        return float4(v, v, v, 1.0);
    }
    return float4(0.0, 0.0, 0.0, 1.0);   // Black (mode 0)
}

// The exact test D3D11DualCompositor's sampleFit performs minus the
// texture read: is `px` inside the aspect-fit of srcSize into the
// rect at rectOrigin/rectSize? Kept step-for-step identical so the
// bounds edge lands on the same canvas pixel as the dual compositor's
// discard edge.
bool insideFit(float2 px, float2 rectOrigin, float2 rectSize, float2 srcSize)
{
    if (srcSize.x <= 0.0 || srcSize.y <= 0.0) return false;
    float2 rectPxPos = px - rectOrigin;
    if (rectPxPos.x < 0.0 || rectPxPos.x >= rectSize.x ||
        rectPxPos.y < 0.0 || rectPxPos.y >= rectSize.y) {
        return false;
    }
    float scale = min(rectSize.x / srcSize.x, rectSize.y / srcSize.y);
    float2 scaledSrc = srcSize * scale;
    float2 offset    = (rectSize - scaledSrc) * 0.5;
    float2 inSrcPx   = (rectPxPos - offset) / scale;
    return !(inSrcPx.x < 0.0 || inSrcPx.x >= srcSize.x ||
             inSrcPx.y < 0.0 || inSrcPx.y >= srcSize.y);
}

// Is this destination pixel covered by media? Single layouts use the
// CPU-computed rect with the SAME comparison the source sampling
// uses; dual layouts map dst → canvas (the present blit's aspect-fit)
// and repeat the canvas compositor's per-mode region layout.
bool insideMedia(float2 fragPx)
{
    if (hasLayout == 0) return true;
    if (layoutMode == 0) {
        return aValid != 0 &&
               fragPx.x >= mediaRect.x && fragPx.x < mediaRect.x + mediaRect.z &&
               fragPx.y >= mediaRect.y && fragPx.y < mediaRect.y + mediaRect.w;
    }
    if (canvasSize.x <= 0.0 || canvasSize.y <= 0.0) return false;
    float  scale  = min(dstSize.x / canvasSize.x, dstSize.y / canvasSize.y);
    float2 scaled = canvasSize * scale;
    float2 offset = (dstSize - scaled) * 0.5;
    float2 cPx    = (fragPx - offset) / scale;
    if (cPx.x < 0.0 || cPx.x >= canvasSize.x ||
        cPx.y < 0.0 || cPx.y >= canvasSize.y) {
        return false;
    }
    const bool aOk = aValid != 0;
    const bool bOk = bValid != 0;
    const float2 zero = float2(0.0, 0.0);
    if (layoutMode == 1) {
        // Side-by-side: A left half, B right half.
        const float  halfW = canvasSize.x * 0.5;
        const float2 halfRect = float2(halfW, canvasSize.y);
        if (cPx.x < halfW) return aOk && insideFit(cPx, zero, halfRect, srcSizeA);
        return bOk && insideFit(cPx, float2(halfW, 0.0), halfRect, srcSizeB);
    } else if (layoutMode == 2) {
        // Wipe: A where canvas u < splitPos, else B; both full-canvas fits.
        if (cPx.x / canvasSize.x < splitPos) {
            return aOk && insideFit(cPx, zero, canvasSize, srcSizeA);
        }
        return bOk && insideFit(cPx, zero, canvasSize, srcSizeB);
    }
    // Difference: union of both full-canvas fits (max(a.a, b.a)).
    return (aOk && insideFit(cPx, zero, canvasSize, srcSizeA)) ||
           (bOk && insideFit(cPx, zero, canvasSize, srcSizeB));
}

// ---- output encoding (verbatim from d3d11_annotation_renderer.cpp) ----
float3 srgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    float3 s  = step(0.04045, c);
    return lerp(lo, hi, s);
}

static const float3x3 kBt709ToBt2020 = float3x3(
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163914, 0.0880132, 0.8955950
);

float3 pqEncode(float3 lin)
{
    // ST.2084 inverse-EOTF. lin is normalized so that 1.0 = 10,000 nits.
    const float m1 = 0.1593017578125;   // 2610 / 16384
    const float m2 = 78.84375;          // 2523 /    32
    const float c1 =  0.8359375;        // 3424 /  4096
    const float c2 = 18.8515625;        // 2413 /   128
    const float c3 = 18.6875;           // 2392 /   128
    float3 Y   = max(lin, 0.0);
    float3 Ym1 = pow(Y, m1);
    float3 num = c1 + c2 * Ym1;
    float3 den = 1.0 + c3 * Ym1;
    return pow(num / den, m2);
}

// sRGB-space fill → swapchain encoding.
float3 encodeFill(float3 c)
{
    if (encoding == 1) {
        return srgbToLinear(c) * (refNits / 80.0);       // scRGB: 1.0 = 80 nits
    } else if (encoding == 2) {
        float3 lin2020 = mul(kBt709ToBt2020, srgbToLinear(c));
        return pqEncode(lin2020 * (refNits / 10000.0));
    }
    return c;
}

// The background under this pixel: mode fill, blended toward the
// theme target outside the media, then encoded for the swapchain.
float3 fillColor(float2 fragPx)
{
    float3 fill = backgroundColor(fragPx).rgb;
    if (!insideMedia(fragPx)) {
        fill = lerp(fill, target.rgb, saturate(target.a));
    }
    return encodeFill(fill);
}

float4 PSMain(VsOut input) : SV_TARGET
{
    float2 fragPx = input.uv * dstSize;
    const float srcGain = (applyBrightness != 0) ? brightness : 1.0;

    // Overlay mode (viewport notice card): blend the source STRAIGHT over
    // whatever is already in the RTV via the hardware src-over blend
    // state, and emit fully-transparent outside the source rect so the
    // existing viewport background shows through the card's rounded
    // corners / margin. Mirrors MetalCompositor's present pass
    // (enableSrcOverBlending + discard on alpha==0). No bg fill, no
    // border here — the card carries its own rounded border.
    //
    // Source-only mode (2) is the same fragment with the opaque blend
    // state bound: the OCIO intermediate receives the straight picture
    // and alpha 0 everywhere else, so the present pass can composite
    // it over the real (post-OCIO) background.
    if (overlayMode != 0) {
        if (hasSrc != 0 &&
            fragPx.x >= fitRectMin.x && fragPx.x < fitRectMin.x + fitRectSize.x &&
            fragPx.y >= fitRectMin.y && fragPx.y < fitRectMin.y + fitRectSize.y)
        {
            float2 srcUv = (fragPx - fitRectMin) / fitRectSize;
            float4 v = src.Sample(srcSamp, rotatedSrcUv(srcUv));
            return float4(v.rgb * srcGain, v.a);
        }
        return float4(0.0, 0.0, 0.0, 0.0);   // leave RTV untouched
    }

    // Edge frame — painted over everything when borderPx > 0 (corner-
    // overlay thumbnails). The main video pass leaves borderPx at 0,
    // so this is a no-op there.
    if (borderPx > 0.0 &&
        (fragPx.x < borderPx || fragPx.x > dstSize.x - borderPx ||
         fragPx.y < borderPx || fragPx.y > dstSize.y - borderPx)) {
        return float4(borderColor * srcGain, 1.0);
    }
    float3 bg = fillColor(fragPx);
    if (hasSrc != 0 &&
        fragPx.x >= fitRectMin.x && fragPx.x <  fitRectMin.x + fitRectSize.x &&
        fragPx.y >= fitRectMin.y && fragPx.y <  fitRectMin.y + fitRectSize.y)
    {
        float2 srcUv = (fragPx - fitRectMin) / fitRectSize;
        float4 v = src.Sample(srcSamp, rotatedSrcUv(srcUv));
        // Source-over-alpha composite onto the bg fill, then write
        // fully-opaque to the swapchain so DComp doesn't compose
        // transparent video pixels against whatever's underneath the
        // player surface (which currently reads as a flat blue tint).
        // The user-picked Background mode shows through where the
        // video has alpha < 1. Brightness scales the picture only —
        // the fill has to keep matching the Qt chrome.
        float3 rgb = v.rgb * srcGain * v.a + bg * (1.0 - v.a);
        return float4(rgb, 1.0);
    }
    return float4(bg, 1.0);
}
)";

// Loading spinner — full-viewport dark fill + a ring of 8 rotating
// dots tinted with the cobalt accent. Self-contained (no source
// texture). Verbatim port of the Metal spin_fs in dual_compositor.mm.
// Reuses the compositor's fullscreen-triangle VS (VsOut.uv).
constexpr const char *kSpinnerPsHlsl = R"(
cbuffer SpinConstants : register(b0) {
    float2 spinDstSize;   // viewport pixels
    float  spinTime;      // seconds since the spinner started
    float  _spinPad;
};

struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 SpinPSMain(VsOut input) : SV_TARGET
{
    float4 bg = float4(0.0863, 0.0863, 0.0863, 1.0);   // #161616 (Theme.bg)
    float2 px = input.uv * spinDstSize;
    float2 c  = spinDstSize * 0.5;
    float  r  = min(spinDstSize.x, spinDstSize.y) * 0.03;
    float  dotR = r * 0.18;
    const int N = 8;
    float rot = -spinTime * 6.2831853 / 1.2;   // one turn / 1.2 s
    float minDist = 1e6;
    int   minIdx  = 0;
    for (int i = 0; i < N; ++i) {
        float ang = rot + (6.2831853 * float(i) / float(N));
        float2 dotPos = c + float2(cos(ang), sin(ang)) * r;
        float d = distance(px, dotPos);
        if (d < minDist) { minDist = d; minIdx = i; }
    }
    if (minDist < dotR) {
        float  brightness = 0.35 + 0.65 * (1.0 - float(minIdx) / float(N - 1));
        float3 accent = float3(0.0039, 0.5373, 0.9451);   // #0189f1
        float  fade = smoothstep(dotR, dotR * 0.55, minDist);
        return float4(lerp(bg.rgb, accent * brightness, fade), 1.0);
    }
    return bg;
}
)";

ComPtr<ID3DBlob> compile(const char *src, const char *entry, const char *target)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(
        src, std::strlen(src), entry, nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        qCritical("D3D11Compositor: D3DCompile %s failed (hr=0x%08lX)\n%s",
                  entry, static_cast<unsigned long>(hr),
                  errors ? static_cast<const char *>(errors->GetBufferPointer())
                         : "(no error blob)");
        return nullptr;
    }
    return code;
}

struct SpinnerCB {
    float dstSize[2];   // 0 : 8
    float time;         // 8 : 12
    float pad;          // 12 : 16
};
static_assert(sizeof(SpinnerCB) == 16,
              "SpinnerCB must be 16-byte aligned for D3D11 cbuffer.");

// Constant buffer layout — must align with HLSL `cbuffer Constants`.
// HLSL packs in 16-byte rows; we pad explicitly to match.
struct CompositorCB {
    float dstSize[2];     // 8
    float fitMin[2];      // 16
    float fitSize[2];     // 24
    float borderPx;       // 28
    float pad0;           // 32
    int   hasSrc;         // 36
    int   bgMode;         // 40
    int   overlayMode;    // 44  (0 = bg-fill composite; 1 = src-over the RTV;
                          //      2 = source-only into an intermediate)
    int   rotQ;           // 48  display rotation, quarter-turns CW {0..3}
    float brightness;     // 52
    float borderColor[3]; // 64
    // ---- media-bounds fill + output encoding ----
    float canvasSize[2];  // 72
    float srcSizeA[2];    // 80
    float srcSizeB[2];    // 88
    int   layoutMode;     // 92
    float splitPos;       // 96
    int   aValid;         // 100
    int   bValid;         // 104
    int   hasLayout;      // 108
    int   encoding;       // 112
    float target[4];      // 128  rgb = outside target (sRGB), a = strength
    float mediaRect[4];   // 144  layoutMode 0: min.xy, size.zw (dst px)
    float refNits;        // 148
    int   applyBrightness;// 152
    float pad1[2];        // 160
};
static_assert(sizeof(CompositorCB) == 160,
              "CompositorCB must match HLSL packing exactly.");
static_assert(sizeof(CompositorCB) % 16 == 0,
              "CompositorCB must be 16-byte aligned for D3D11 cbuffer.");

// Aspect-fit `srcW×srcH` into `dstW×dstH`. ONE implementation shared
// by the source fit and the media-bounds rect so both produce bit-
// identical floats for the same inputs — the bounds edge must land on
// the same pixel as the picture's edge (no seam).
struct FitRect { float x, y, w, h; };
FitRect aspectFit(float dstW, float dstH, float srcW, float srcH)
{
    FitRect r{ 0.0f, 0.0f, dstW, dstH };
    if (srcW > 0.0f && srcH > 0.0f) {
        const float srcAspect = srcW / srcH;
        const float dstAspect = dstW / dstH;
        if (srcAspect > dstAspect) {
            r.w = dstW;
            r.h = dstW / srcAspect;
        } else {
            r.h = dstH;
            r.w = dstH * srcAspect;
        }
    }
    r.x = (dstW - r.w) * 0.5f;
    r.y = (dstH - r.h) * 0.5f;
    return r;
}

// Media-bounds policy (ported from metal_compositor.mm, then retuned
// with chris on Windows 2026-09-15): outside the media rect the fill
// is mixed toward Theme.bg (#161616) by `strength`. Strength defaults:
//   Black        0.0 — solid black everywhere (plain-black viewers)
//   DarkGray     1.0 — #1f1f1f inside the media, #161616 outside
//   DarkChecker  0.7 — the checker ghosts through the grey outside
//   LightChecker 0.7   (chris, 2.3.2)
// Tunable at launch via QCV_BOUNDS_MIX ("0.7" applies to all four;
// "1.0,0.5,1.0,0.7" is per mode: black, darkgray, darkChecker,
// lightChecker). NOTE: macOS still has the earlier policy (black
// blended, DarkGray #161616 inside → #1f1f1f outside) — mirror there.
constexpr float kThemeBg      = 22.0f / 255.0f;   // #161616 Theme.bg

struct BoundsPolicy {
    float target[3];
    float strength;
};

const std::array<float, 4> &boundsStrengths()
{
    static const std::array<float, 4> strengths = [] {
        std::array<float, 4> s = { 0.0f, 1.0f, 0.7f, 0.7f };   // tuned 2026-09-15
        const QByteArray env = qgetenv("QCV_BOUNDS_MIX");
        if (!env.isEmpty()) {
            const QList<QByteArray> parts = env.split(',');
            if (parts.size() == 1) {
                bool ok = false;
                const float v = parts[0].trimmed().toFloat(&ok);
                if (ok) s.fill(std::clamp(v, 0.0f, 1.0f));
            } else {
                for (int i = 0; i < std::min<int>(4, parts.size()); ++i) {
                    bool ok = false;
                    const float v = parts[i].trimmed().toFloat(&ok);
                    if (ok) s[i] = std::clamp(v, 0.0f, 1.0f);
                }
            }
            qInfo("D3D11Compositor: QCV_BOUNDS_MIX override → "
                  "black=%.2f darkgray=%.2f darkChecker=%.2f lightChecker=%.2f",
                  s[0], s[1], s[2], s[3]);
        }
        return s;
    }();
    return strengths;
}

BoundsPolicy boundsPolicyFor(int mode)
{
    const int m = std::max(0, std::min(3, mode));
    BoundsPolicy p;
    p.target[0] = p.target[1] = p.target[2] = kThemeBg;
    p.strength  = boundsStrengths()[static_cast<size_t>(m)];
    return p;
}

} // namespace

struct D3D11Compositor::Impl {
    ComPtr<ID3D11VertexShader>   vs;
    ComPtr<ID3D11PixelShader>    ps;
    ComPtr<ID3D11PixelShader>    spinnerPs;
    ComPtr<ID3D11SamplerState>   sampler;
    ComPtr<ID3D11Buffer>         cbuf;
    ComPtr<ID3D11Buffer>         spinnerCbuf;
    ComPtr<ID3D11RasterizerState> rasterState;
    ComPtr<ID3D11BlendState>     blendState;       // opaque (BlendEnable=FALSE)
    ComPtr<ID3D11BlendState>     blendStateAlpha;  // non-premult src-over
    float                        brightness = 1.0f;
    float                        refNits    = 200.0f;   // Guide 06 D11 default
    bool initialized = false;
};

D3D11Compositor::D3D11Compositor() : m_impl(std::make_unique<Impl>()) {}

void D3D11Compositor::setBrightness(float brightness)
{
    if (m_impl) m_impl->brightness = brightness;
}

void D3D11Compositor::setReferenceLuminance(float nits)
{
    if (m_impl && nits > 0.0f) m_impl->refNits = nits;
}

D3D11Compositor::OutputEncoding D3D11Compositor::encodingForFormat(int dxgiFormat)
{
    // Same format-keyed choice as D3D11AnnotationRenderer::initialize.
    switch (static_cast<DXGI_FORMAT>(dxgiFormat)) {
        case DXGI_FORMAT_R10G10B10A2_UNORM:  return OutputEncoding::Hdr10Pq;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return OutputEncoding::ScRgbLinear;
        default:                             return OutputEncoding::Sdr;
    }
}
D3D11Compositor::~D3D11Compositor() { shutdown(); }
bool D3D11Compositor::isInitialized() const { return m_impl && m_impl->initialized; }

bool D3D11Compositor::initialize()
{
    if (m_impl->initialized) return true;
    if (!D3D11DeviceManager::instance().isInitialized()) {
        qCritical("D3D11Compositor::initialize: device manager not ready");
        return false;
    }
    auto *device = static_cast<ID3D11Device *>(
        D3D11DeviceManager::instance().device());

    ComPtr<ID3DBlob> vsBlob = compile(kVsHlsl, "VSMain", "vs_5_0");
    ComPtr<ID3DBlob> psBlob = compile(kPsHlsl, "PSMain", "ps_5_0");
    if (!vsBlob || !psBlob) return false;

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                            vsBlob->GetBufferSize(),
                                            nullptr, m_impl->vs.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateVertexShader failed");
        return false;
    }
    if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(),
                                            psBlob->GetBufferSize(),
                                            nullptr, m_impl->ps.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreatePixelShader failed");
        return false;
    }

    ComPtr<ID3DBlob> spinBlob = compile(kSpinnerPsHlsl, "SpinPSMain", "ps_5_0");
    if (!spinBlob) return false;
    if (FAILED(device->CreatePixelShader(spinBlob->GetBufferPointer(),
                                            spinBlob->GetBufferSize(),
                                            nullptr, m_impl->spinnerPs.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreatePixelShader (spinner) failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, m_impl->sampler.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateSamplerState failed");
        return false;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(CompositorCB);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, m_impl->cbuf.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateBuffer (cbuf) failed");
        return false;
    }

    D3D11_BUFFER_DESC scbd{};
    scbd.ByteWidth      = sizeof(SpinnerCB);
    scbd.Usage          = D3D11_USAGE_DYNAMIC;
    scbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    scbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&scbd, nullptr, m_impl->spinnerCbuf.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateBuffer (spinnerCbuf) failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, m_impl->rasterState.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateRasterizerState failed");
        return false;
    }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, m_impl->blendState.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateBlendState failed");
        return false;
    }

    // Non-premultiplied source-over for the overlay (viewport notice
    // card) path — exact match for MetalCompositor's present blend
    // (metal_compositor.mm:419-425): RGB = src.a·src + (1-src.a)·dst;
    // alpha = src.a + (1-src.a)·dst.a (stays 1 over the opaque bg, so
    // the swapchain remains fully opaque and DComp doesn't composite
    // the desktop/blue tint through).
    D3D11_BLEND_DESC bda{};
    bda.RenderTarget[0].BlendEnable           = TRUE;
    bda.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bda.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bda.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bda.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bda.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bda.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bda.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bda, m_impl->blendStateAlpha.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateBlendState (alpha) failed");
        return false;
    }

    m_impl->initialized = true;
    qInfo("D3D11Compositor: initialized");
    return true;
}

void D3D11Compositor::shutdown()
{
    if (!m_impl) return;
    m_impl->blendState.Reset();
    m_impl->blendStateAlpha.Reset();
    m_impl->rasterState.Reset();
    m_impl->cbuf.Reset();
    m_impl->spinnerCbuf.Reset();
    m_impl->sampler.Reset();
    m_impl->spinnerPs.Reset();
    m_impl->ps.Reset();
    m_impl->vs.Reset();
    m_impl->initialized = false;
}

void D3D11Compositor::renderSingle(void *ctxVoid, void *srcSrvVoid,
                                     int dstW, int dstH,
                                     int srcW, int srcH,
                                     int bgMode,
                                     float borderPx,
                                     float borderR, float borderG, float borderB,
                                     bool overlayBlend,
                                     int rotQuarters,
                                     const PassOptions *opts)
{
    if (!m_impl || !m_impl->initialized) return;
    if (dstW <= 0 || dstH <= 0) return;

    auto *ctx    = static_cast<ID3D11DeviceContext *>(ctxVoid);
    auto *srcSrv = static_cast<ID3D11ShaderResourceView *>(srcSrvVoid);

    const float dstWf = static_cast<float>(dstW);
    const float dstHf = static_cast<float>(dstH);

    // Compute aspect-fit rect (full destination when there's no source).
    const FitRect fit = srcSrv
        ? aspectFit(dstWf, dstHf, static_cast<float>(srcW), static_cast<float>(srcH))
        : FitRect{ 0.0f, 0.0f, dstWf, dstHf };

    const bool sourceOnly = opts && opts->sourceOnly;

    // Map + update constant buffer.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(m_impl->cbuf.Get(), 0,
                          D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    CompositorCB cb{};
    cb.dstSize[0] = dstWf;
    cb.dstSize[1] = dstHf;
    cb.fitMin[0]  = fit.x;
    cb.fitMin[1]  = fit.y;
    cb.fitSize[0] = fit.w;
    cb.fitSize[1] = fit.h;
    cb.hasSrc     = srcSrv ? 1 : 0;
    cb.bgMode     = bgMode;
    cb.overlayMode = overlayBlend ? 1 : (sourceOnly ? 2 : 0);
    cb.rotQ       = rotQuarters & 3;
    cb.brightness = m_impl->brightness;
    cb.applyBrightness = (!opts || opts->applyBrightness) ? 1 : 0;
    cb.borderPx       = borderPx;
    cb.borderColor[0] = borderR;
    cb.borderColor[1] = borderG;
    cb.borderColor[2] = borderB;

    // Media-bounds fill + swapchain encoding (present passes only).
    {
        const int clampedMode = std::max(0, std::min(3, bgMode));
        const BoundsPolicy policy = boundsPolicyFor(clampedMode);
        cb.target[0] = policy.target[0];
        cb.target[1] = policy.target[1];
        cb.target[2] = policy.target[2];
        cb.target[3] = policy.strength;
        cb.refNits   = m_impl->refNits;
        cb.encoding  = opts ? static_cast<int>(opts->encoding) : 0;

        const BackgroundLayout *layout = opts ? opts->layout : nullptr;
        if (layout && layout->canvasW > 0 && layout->canvasH > 0
            && (layout->aValid || layout->bValid)) {
            cb.canvasSize[0] = static_cast<float>(layout->canvasW);
            cb.canvasSize[1] = static_cast<float>(layout->canvasH);
            cb.srcSizeA[0]   = static_cast<float>(layout->srcAW);
            cb.srcSizeA[1]   = static_cast<float>(layout->srcAH);
            cb.srcSizeB[0]   = static_cast<float>(layout->srcBW);
            cb.srcSizeB[1]   = static_cast<float>(layout->srcBH);
            cb.layoutMode    = std::max(0, std::min(3, layout->layoutMode));
            cb.splitPos      = layout->splitPos;
            cb.aValid        = layout->aValid ? 1 : 0;
            cb.bValid        = layout->bValid ? 1 : 0;
            cb.hasLayout     = 1;
            if (cb.layoutMode == 0) {
                // Single layout: compute the media rect on the CPU with
                // the SAME aspectFit the source pass used. When the
                // canvas is presented 1:1 (the normal case — the OCIO
                // intermediate is swapchain-sized) the canvas rect is
                // taken as exactly the destination, so the media rect
                // is bit-identical to the source pass's fit rect and
                // the shader's comparison lands on the same pixel.
                FitRect canvas{ 0.0f, 0.0f, dstWf, dstHf };
                if (layout->canvasW != dstW || layout->canvasH != dstH) {
                    canvas = aspectFit(dstWf, dstHf,
                                       cb.canvasSize[0], cb.canvasSize[1]);
                }
                const FitRect media = aspectFit(canvas.w, canvas.h,
                                                cb.srcSizeA[0], cb.srcSizeA[1]);
                cb.mediaRect[0] = canvas.x + media.x;
                cb.mediaRect[1] = canvas.y + media.y;
                cb.mediaRect[2] = media.w;
                cb.mediaRect[3] = media.h;
            }
        }
    }
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(m_impl->cbuf.Get(), 0);

    // Pipeline state.
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_impl->vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_impl->ps.Get(), nullptr, 0);
    ID3D11Buffer *cb0 = m_impl->cbuf.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb0);
    ID3D11SamplerState *s0 = m_impl->sampler.Get();
    ctx->PSSetSamplers(0, 1, &s0);
    ID3D11ShaderResourceView *srvs[1] = { srcSrv };
    ctx->PSSetShaderResources(0, 1, srvs);
    ctx->RSSetState(m_impl->rasterState.Get());
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(
        overlayBlend ? m_impl->blendStateAlpha.Get() : m_impl->blendState.Get(),
        blendFactor, 0xFFFFFFFF);

    ctx->Draw(3, 0);

    // Unbind the source SRV so we can use the same slot as an upload
    // destination on the next frame without a hazard warning.
    ID3D11ShaderResourceView *nullSrv[1] = { nullptr };
    ctx->PSSetShaderResources(0, 1, nullSrv);
}

void D3D11Compositor::renderCornerOverlay(void *ctxVoid, void *srcSrvVoid,
                                            int srcW, int srcH,
                                            int dstW, int dstH,
                                            int corner,
                                            float overlayFrac,
                                            float marginPx,
                                            int rotQuarters)
{
    if (!m_impl || !m_impl->initialized) return;
    if (!ctxVoid || !srcSrvVoid) return;
    if (dstW <= 0 || dstH <= 0) return;
    if (srcW <= 0 || srcH <= 0) return;

    overlayFrac = std::clamp(overlayFrac, 0.05f, 0.5f);
    if (marginPx < 0.0f) marginPx = 0.0f;

    // Box width = fraction of canvas width; height follows the source
    // aspect (landscape thumbs get a wider box). Cap height at the box
    // width so a tall portrait thumb is never taller than it is wide —
    // the source then pillarboxes inside the resulting square box.
    const float boxW = static_cast<float>(dstW) * overlayFrac;
    const float aspect =
        static_cast<float>(srcW) / static_cast<float>(std::max(srcH, 1));
    float boxH = boxW / std::max(aspect, 0.0001f);
    if (boxH > boxW) boxH = boxW;

    // D3D11 viewport origin = top-left; corner=0 (BL) and corner=1
    // (BR) sit at the bottom of the canvas with marginPx inset.
    float originX = marginPx;
    float originY = static_cast<float>(dstH) - boxH - marginPx;
    if (corner == 1) {
        originX = static_cast<float>(dstW) - boxW - marginPx;
    } else if (corner == 2) { // center (viewport notice card)
        originX = (static_cast<float>(dstW) - boxW) * 0.5f;
        originY = (static_cast<float>(dstH) - boxH) * 0.5f;
    }
    if (originX < 0.0f) originX = 0.0f;
    if (originY < 0.0f) originY = 0.0f;

    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxVoid);

    // Save caller's viewport so downstream passes (screenshot,
    // present) see the canvas extent they expect.
    UINT prevCount = 1;
    D3D11_VIEWPORT prev{};
    ctx->RSGetViewports(&prevCount, &prev);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = originX;
    vp.TopLeftY = originY;
    vp.Width    = boxW;
    vp.Height   = boxH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Reuse renderSingle: it computes its own aspect-fit math with
    // the box dims as the canvas, draws the same fullscreen-triangle
    // (rasterizer clips to the corner viewport), and unbinds the SRV
    // afterward. A faint grey frame (borderPx > 0) keeps the thumb
    // from blending into the viewport behind it.
    renderSingle(ctxVoid, srcSrvVoid,
                 static_cast<int>(boxW), static_cast<int>(boxH),
                 srcW, srcH,
                 /*bgMode=*/0,
                 // No compositor frame for the centered notice card
                 // (corner==2): it carries its own rounded border in the
                 // source image; a second rectangular frame reads as a
                 // doubled border. Hover thumbs (0/1) keep the 2 px frame.
                 /*borderPx=*/(corner == 2 ? 0.0f : 2.0f),
                 /*borderRGB ≈ #474747=*/0.28f, 0.28f, 0.28f,
                 // Notice card (corner==2): src-over the live viewport
                 // background so its rounded corners stay transparent —
                 // matches macOS. Hover thumbs stay opaque tiles.
                 /*overlayBlend=*/(corner == 2),
                 rotQuarters);

    ctx->RSSetViewports(1, &prev);
}

void D3D11Compositor::renderSpinner(void *ctxVoid, int dstW, int dstH,
                                     float timeSeconds)
{
    if (!m_impl || !m_impl->initialized) return;
    if (!ctxVoid || dstW <= 0 || dstH <= 0) return;
    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxVoid);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(m_impl->spinnerCbuf.Get(), 0,
                          D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    SpinnerCB cb{};
    cb.dstSize[0] = static_cast<float>(dstW);
    cb.dstSize[1] = static_cast<float>(dstH);
    cb.time       = timeSeconds;
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(m_impl->spinnerCbuf.Get(), 0);

    // Fullscreen triangle, opaque fill — no source texture/sampler.
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_impl->vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_impl->spinnerPs.Get(), nullptr, 0);
    ID3D11Buffer *cb0 = m_impl->spinnerCbuf.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb0);
    ctx->RSSetState(m_impl->rasterState.Get());
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(m_impl->blendState.Get(), blendFactor, 0xFFFFFFFF);
    ctx->Draw(3, 0);
}

} // namespace qcv
