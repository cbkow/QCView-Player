// D3D11ScopeRenderer — see header.

#include "d3d11_scope_renderer.h"

#include "color/ocio_chain_builder.h"
#include "color/ocio_config_manager.h"
#include "d3d11_device_manager.h"
#include "d3d11_ocio_luts.h"

#include <QElapsedTimer>
#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace qcv {

namespace {

constexpr int kRingSlots = 3;

constexpr const char *kVsHlsl = R"(
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VsOut VSMain(uint id : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

// Tap pixel shader; the converted variant splices OCIOScope in.
constexpr const char *kTapHead = R"(
Texture2D    uSrc        : register(t0);
SamplerState uSrcSampler : register(s0);
)";
constexpr const char *kTapMain = R"(
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(VsOut input) : SV_TARGET
{
    float4 c = uSrc.Sample(uSrcSampler, input.uv);
    float3 rgb = c.rgb;
    QCV_SCOPE_CONVERT
    return float4(rgb, c.a);
}
)";

// Shared math for the compute kernels and the peak pass. Mirrors
// scope_math::classify() / bin().
constexpr const char *kScopeCommon = R"(
static const int G = 512;
static const int COPIES = 4;

float qs_spow(float x, float k) { return sign(x) * pow(abs(x), k); }

float qs_pq(float y)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(max(y, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float3 qs_ycc(float3 e, int m)
{
    float kr = 0.2126, kb = 0.0722;
    if (m == 2)      { kr = 0.2627; kb = 0.0593; }
    else if (m == 0) { kr = 0.299;  kb = 0.114;  }
    float y = kr * e.r + (1.0 - kr - kb) * e.g + kb * e.b;
    return float3(y, (e.b - y) / (2.0 * (1.0 - kb)), (e.r - y) / (2.0 * (1.0 - kr)));
}

// One pixel in scope space (scope_math::classify): (Y′, Cb, Cr), outside
// the scale's gamut, HDR luminance in nits, brightest channel.
void qs_classify(float3 rgb, float4 to0, float4 to1, float4 to2, float4 p0, float4 p1,
                 float4 p2, out float3 ycc, out bool outside, out float nits, out float chan)
{
    outside = false;
    nits = 0.0;
    if (p0.x > 0.5) {
        float3 l = float3(dot(to0.xyz, rgb), dot(to1.xyz, rgb), dot(to2.xyz, rgb));
        if (p0.y < 0.5) {
            float3 l7 = float3( 1.6604910 * l.r - 0.5876411 * l.g - 0.0728499 * l.b,
                               -0.1245505 * l.r + 1.1328999 * l.g - 0.0083494 * l.b,
                               -0.0181508 * l.r - 0.1005789 * l.g + 1.1187297 * l.b);
            outside = any(l7 < -0.002);
            float3 e = float3(qs_spow(l7.r, 1.0 / 2.4), qs_spow(l7.g, 1.0 / 2.4),
                              qs_spow(l7.b, 1.0 / 2.4));
            chan = max(e.r, max(e.g, e.b));
            ycc = qs_ycc(e, 1);
        } else {
            outside = any(l < -0.002);
            // p2.w = nits per 1.0 linear (100 display-referred, 203 SDR /
            // scene white) — scope_math::classify.
            float white = p2.w > 0.0 ? p2.w : 100.0;
            nits = white * dot(float3(0.2627, 0.6780, 0.0593), l);
            chan = white * max(l.r, max(l.g, l.b));
            float k = white / 10000.0;
            float3 e = sign(l) * float3(qs_pq(abs(l.r) * k), qs_pq(abs(l.g) * k),
                                        qs_pq(abs(l.b) * k));
            ycc = qs_ycc(e, 2);
        }
    } else {
        float3 e = rgb;
        if (p1.x > 0.5) {
            e = float3(qs_spow(e.r, 1.0 / 2.4), qs_spow(e.g, 1.0 / 2.4), qs_spow(e.b, 1.0 / 2.4));
        }
        chan = max(e.r, max(e.g, e.b));
        ycc = qs_ycc(e, (int)p0.w);
    }
}

// A peak value as orderable uint bits: NaN (tested on the bits — the
// compiler may fold isnan), negatives and −0 → 0, capped at 1e6.
uint qs_peakBits(float v)
{
    uint b = asuint(v);
    if ((b & 0x7fffffffu) > 0x7f800000u) return 0u;
    v = min(v, 1e6);
    return v > 0.0 ? asuint(v) : 0u;
}
)";

constexpr const char *kAccumCs = R"(
Texture2D<float4>   tapTex : register(t0);
RWByteAddressBuffer grid   : register(u0);
RWByteAddressBuffer oog    : register(u1);
cbuffer ScopeAccumCb : register(b0) { float4 to0, to1, to2; float4 p0; float4 p1; float4 p2; };

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 tg : SV_GroupID)
{
    uint w = (uint)p1.y, h = (uint)p1.z;
    if (id.x >= w || id.y >= h) return;
    float4 c = tapTex.Load(int3(id.xy, 0));
    if (c.a <= 0.0) return;
    float3 ycc;
    bool outside;
    float nits, chan;
    qs_classify(c.rgb, to0, to1, to2, p0, p1, p2, ycc, outside, nits, chan);
    int bx, by;
    if (p2.x > 0.5) {
        // Waveform: column × Y′ / nits (scope_math::bin / waveformRange).
        bool hdr = p0.x > 0.5 && p0.y > 0.5;
        float lo = hdr ? 0.0 : -0.1, hi = hdr ? p2.y : 1.1;
        if (hdr) ycc.x = nits;
        outside = hdr ? ycc.x > hi + 1e-3 : (ycc.x > 1.0 + 1e-4 || ycc.x < -1e-4);
        bx = clamp((int)floor(((float)id.x + 0.5) / (float)w * (float)G), 0, G - 1);
        by = clamp((int)floor((hi - ycc.x) / (hi - lo) * (float)G), 0, G - 1);
    } else {
        float zoom = p0.z;
        bx = clamp((int)floor((ycc.y * zoom + 0.5) * (float)G), 0, G - 1);
        by = clamp((int)floor((0.5 - ycc.z * zoom) * (float)G), 0, G - 1);
    }
    uint side = (uint)p1.w;
    uint copy = (tg.x + tg.y) % (uint)COPIES;
    uint cell = (uint)(by * G + bx);
    uint dummy;
    grid.InterlockedAdd(((side * (uint)COPIES + copy) * (uint)(G * G) + cell) * 4, 1u, dummy);
    if (outside) oog.InterlockedAdd((side * (uint)(G * G) + cell) * 4, 1u, dummy);
}
)";

// Waveform peak pass (after kTapHead + OCIO + kScopeCommon): every source
// pixel, InterlockedMax of the level and the brightest channel — float
// bits, non-negative, so uint order = float order. No render target.
constexpr const char *kPeakPs = R"(
RWByteAddressBuffer peaks : register(u0);
cbuffer ScopeAccumCb : register(b0) { float4 to0, to1, to2; float4 p0; float4 p1; float4 p2; };
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
void PSMain(VsOut input)
{
    float4 c = uSrc.Sample(uSrcSampler, input.uv);
    if (c.a <= 0.0) return;
    float3 rgb = c.rgb;
    QCV_SCOPE_CONVERT
    float3 ycc;
    bool outside;
    float nits, chan;
    qs_classify(rgb, to0, to1, to2, p0, p1, p2, ycc, outside, nits, chan);
    bool hdr = p0.x > 0.5 && p0.y > 0.5;
    uint side = (uint)p1.w;
    uint prev;
    peaks.InterlockedMax(side * 8, qs_peakBits(hdr ? nits : ycc.x), prev);
    peaks.InterlockedMax(side * 8 + 4, qs_peakBits(chan), prev);
}
)";

constexpr const char *kDrawCs = R"(
RWByteAddressBuffer grid    : register(u0);
RWByteAddressBuffer oog     : register(u1);
RWByteAddressBuffer outPx   : register(u2);
cbuffer ScopeDrawCb : register(b0) { float4 d0; float4 d1; };

uint countAt(int s, int x, int y)
{
    uint c = 0u;
    uint cell = (uint)(y * G + x);
    for (int k = 0; k < COPIES; ++k) c += grid.Load(((uint)(s * COPIES + k) * (uint)(G * G) + cell) * 4);
    return c;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)G || id.y >= (uint)G) return;
    uint cell = id.y * (uint)G + id.x;
    int sides = (int)d0.w;
    float3 col = float3(0, 0, 0);
    float alpha = 0.0;
    for (int s = 0; s < sides; ++s) {
        uint c = countAt(s, (int)id.x, (int)id.y);
        uint o = oog.Load(((uint)s * (uint)(G * G) + cell) * 4);
        // Any occupied bin shows at least faintly (a handful of specular
        // pixels must not vanish), then exposure-style build-up.
        float I = c > 0u ? max(1.0 - exp(-d0.x * (float)c), 0.22) : 0.0;
        // 3×3 glow (flat colours land in a single bin).
        float halo = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int x = (int)id.x + dx, y = (int)id.y + dy;
                if ((dx == 0 && dy == 0) || x < 0 || y < 0 || x >= G || y >= G) continue;
                uint n = countAt(s, x, y);
                halo = max(halo, n > 0u ? max(1.0 - exp(-d0.x * (float)n), 0.22) : 0.0);
            }
        }
        I = max(I, 0.6 * halo);
        float3 tint;
        if (sides == 2) {
            tint = s == 0 ? float3(0.30, 0.85, 1.0) : float3(1.0, 0.62, 0.25);
        } else if (d0.y > 0.5 && d1.y < 0.5) {   // colourise: vectorscope only
            float zoom = d1.x;
            float cb = (((float)id.x + 0.5) / (float)G - 0.5) / zoom;
            float cr = (0.5 - ((float)id.y + 0.5) / (float)G) / zoom;
            float3 rgb = saturate(float3(0.5 + 1.5748 * cr,
                                         0.5 - 0.1873 * cb - 0.4681 * cr,
                                         0.5 + 1.8556 * cb));
            float m = max(rgb.r, max(rgb.g, rgb.b));
            tint = lerp(float3(1, 1, 1), rgb / max(m, 1e-3), 0.85);
        } else {
            tint = float3(0.82, 0.95, 0.82);
        }
        if (c > 0u && o * 2u > c) tint = float3(1.0, 0.28, 0.36);
        col += tint * I;
        alpha = max(alpha, I);
    }
    col = min(col, float3(alpha, alpha, alpha));
    uint4 q = (uint4)round(saturate(float4(col, alpha)) * 255.0);
    outPx.Store(cell * 4, q.r | (q.g << 8) | (q.b << 16) | (q.a << 24));
}
)";

struct RawBuffer {
    ComPtr<ID3D11Buffer>              buf;
    ComPtr<ID3D11UnorderedAccessView> uav;
    bool create(ID3D11Device *d, UINT bytes)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes;
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(d->CreateBuffer(&bd, nullptr, buf.GetAddressOf()))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format              = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements  = bytes / 4;
        ud.Buffer.Flags        = D3D11_BUFFER_UAV_FLAG_RAW;
        return SUCCEEDED(d->CreateUnorderedAccessView(buf.Get(), &ud, uav.GetAddressOf()));
    }
    void reset() { uav.Reset(); buf.Reset(); }
};

// A tap-style pixel shader: kTapHead, OCIO's function (converted tiers)
// and `main`, whose QCV_SCOPE_CONVERT marker becomes `call` (or nothing).
std::string tapSource(const std::string &ocioText, std::string main, const std::string &call)
{
    const std::string marker = "QCV_SCOPE_CONVERT";
    main.replace(main.find(marker), marker.size(), call);
    return std::string(kTapHead) + ocioText + main;
}

template <class T>
bool makeCb(ID3D11Device *d, ComPtr<ID3D11Buffer> &out)
{
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(T);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(d->CreateBuffer(&cbd, nullptr, out.GetAddressOf()));
}

template <class T>
void upload(ID3D11DeviceContext *ctx, ID3D11Buffer *cb, const T &v)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &v, sizeof(v));
        ctx->Unmap(cb, 0);
    }
}

} // namespace

struct D3D11ScopeRenderer::Impl {
    ID3D11Device *device = nullptr;
    ComPtr<ID3D11VertexShader>  vs;
    ComPtr<ID3D11PixelShader>   tapSignal;
    ComPtr<ID3D11PixelShader>   peakSignal;
    ComPtr<ID3D11ComputeShader> accum;
    ComPtr<ID3D11ComputeShader> draw;
    ComPtr<ID3D11SamplerState>  sampler;
    ComPtr<ID3D11Buffer>        accumCb, drawCb;

    // Converted tap / peak PS (OCIOScope inside), one set per side — dual
    // sides can be interpreted differently — each keyed on config +
    // colourspace. Same LUTs, slots resolved per shader.
    struct Conversion {
        ComPtr<ID3D11PixelShader> tap;
        ComPtr<ID3D11PixelShader> peak;
        std::vector<LutResource>  luts;
        std::vector<LutResource>  peakLuts;
        QString          key;
        InterchangeSide  side = InterchangeSide::None;
    };
    Conversion conv[2];

    // Tap target (reallocated on size change).
    ComPtr<ID3D11Texture2D>          tapTex;
    ComPtr<ID3D11RenderTargetView>   tapRtv;
    ComPtr<ID3D11ShaderResourceView> tapSrv;
    int tapW = 0, tapH = 0;

    // A staging slot = the G² RGBA8 image, then the four peak words
    // (A level, A channel, B level, B channel).
    RawBuffer grid, oog, peaks, outPx;
    ComPtr<ID3D11Buffer> staging[kRingSlots];
    bool     stagingPending[kRingSlots] = {false, false, false};
    int      stagingSides[kRingSlots]   = {0, 0, 0};   // sides with peaks (0 = none)
    bool     stagingHdr[kRingSlots]     = {false, false, false};
    bool     stagingMeasured[kRingSlots][2] = {};
    quint64  stagingStamp[kRingSlots] = {0, 0, 0};
    quint32  stagingEpoch[kRingSlots] = {0, 0, 0};
    quint64  frameStamp = 0;
    quint64  stagingOrder[kRingSlots]   = {0, 0, 0};
    quint64  submitCount = 0;

    mutable std::mutex imageMutex;
    QImage     latest;
    ScopePeaks latestPeaks;
    quint64    serial = 0;

    ScopeConfig   cfg;
    QElapsedTimer lastUse;

    bool ensureBuffers()
    {
        if (grid.buf) return true;
        const UINT g2 = kScopeGrid * kScopeGrid;
        bool ok = grid.create(device, 2 * kScopeCopies * g2 * 4)
               && oog.create(device, 2 * g2 * 4)
               && peaks.create(device, 4 * 4)
               && outPx.create(device, g2 * 4);
        for (auto &s : staging) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = g2 * 4 + 4 * 4;
            bd.Usage          = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ok = ok && SUCCEEDED(device->CreateBuffer(&bd, nullptr, s.GetAddressOf()));
        }
        if (!ok) { releaseBuffers(); return false; }
        qInfo("D3D11ScopeRenderer: buffers allocated");
        return true;
    }

    void releaseBuffers()
    {
        grid.reset(); oog.reset(); peaks.reset(); outPx.reset();
        for (auto &s : staging) s.Reset();
        for (bool &p : stagingPending) p = false;
        tapSrv.Reset(); tapRtv.Reset(); tapTex.Reset();
        tapW = tapH = 0;
    }

    bool ensureTap(int w, int h)
    {
        if (tapTex && tapW == w && tapH == h) return true;
        tapSrv.Reset(); tapRtv.Reset(); tapTex.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, tapTex.GetAddressOf()))
            || FAILED(device->CreateRenderTargetView(tapTex.Get(), nullptr, tapRtv.GetAddressOf()))
            || FAILED(device->CreateShaderResourceView(tapTex.Get(), nullptr, tapSrv.GetAddressOf()))) {
            tapSrv.Reset(); tapRtv.Reset(); tapTex.Reset();
            return false;
        }
        tapW = w; tapH = h;
        return true;
    }

    // Build (or reuse) side `s`'s converted shaders for `colorspace` (from
    // `configPath`, or the live config when empty).
    // False = unavailable — the caller falls back to Signal for that side.
    bool ensureConverted(OCIOConfigManager *ocio, int s, const QString &colorspace,
                         const QString &configPath)
    {
        if (!ocio || colorspace.isEmpty()) return false;
        Conversion &c = conv[s];
        const QString key = (configPath.isEmpty() ? ocio->configIdentifier() : configPath)
                            + QLatin1Char('|') + colorspace;
        if (key == c.key) return c.tap && c.peak;
        if (conv[1 - s].key == key) {   // the other side already built it
            c = conv[1 - s];
            return c.tap && c.peak;
        }
        c = Conversion{};
        c.key = key;
        InterchangeSide side = InterchangeSide::None;
        OcioChain chain = OcioChainBuilder::buildScope(
            ocio, OcioChainBuilder::Language::Hlsl_Sm_5_0, colorspace, &side, configPath);
        if (!chain.ok) {
            qInfo("D3D11ScopeRenderer: no conversion for '%s' (%s) — Signal",
                  qPrintable(colorspace), qPrintable(chain.errorMessage));
            return false;
        }
        QString err;
        if (!createLuts(device, chain.desc, c.luts, err)) { c.luts.clear(); return false; }
        const std::string ocioText = chain.shaderText.toStdString();
        const std::string call = "rgb = OCIOScope(float4(rgb, 1.0)).rgb;";
        ComPtr<ID3DBlob> blob = compileHlsl(tapSource(ocioText, kTapMain, call), "PSMain", "ps_5_0", &err);
        ComPtr<ID3DBlob> peakBlob =
            blob ? compileHlsl(tapSource(ocioText, std::string(kScopeCommon) + kPeakPs, call),
                               "PSMain", "ps_5_0", &err)
                 : nullptr;
        if (!blob || !peakBlob
            || FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                nullptr, c.tap.GetAddressOf()))
            || FAILED(device->CreatePixelShader(peakBlob->GetBufferPointer(), peakBlob->GetBufferSize(),
                                                nullptr, c.peak.GetAddressOf()))) {
            qWarning("D3D11ScopeRenderer: tap compile failed: %s", qPrintable(err));
            c.tap.Reset();
            c.peak.Reset();
            c.luts.clear();
            return false;
        }
        int byName = 0, byFallback = 0;
        resolveLutSlots(blob.Get(), c.luts, byName, byFallback);
        c.peakLuts = c.luts;
        resolveLutSlots(peakBlob.Get(), c.peakLuts, byName, byFallback);
        c.side = side;
        qInfo("D3D11ScopeRenderer: conversion built for '%s' (%zu LUTs)",
              qPrintable(colorspace), c.luts.size());
        return true;
    }

    // Map any finished staging buffer without waiting; keep the newest.
    void collect(ID3D11DeviceContext *ctx)
    {
        int best = -1;
        for (int s = 0; s < kRingSlots; ++s) {
            if (!stagingPending[s]) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (ctx->Map(staging[s].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) {
                continue;   // still on the GPU
            }
            if (best < 0 || stagingOrder[s] > stagingOrder[best]) {
                const size_t imageBytes = static_cast<size_t>(kScopeGrid) * kScopeGrid * 4;
                QImage img(kScopeGrid, kScopeGrid, QImage::Format_RGBA8888_Premultiplied);
                std::memcpy(img.bits(), m.pData, imageBytes);
                ScopePeaks pk;
                pk.sides = stagingSides[s];
                pk.frameStamp = stagingStamp[s];
                pk.epoch = stagingEpoch[s];
                pk.hdr   = stagingHdr[s];
                float words[4];
                std::memcpy(words, static_cast<const char *>(m.pData) + imageBytes, sizeof(words));
                for (int k = 0; k < pk.sides; ++k) {
                    pk.measured[k] = stagingMeasured[s][k];
                    pk.level[k]    = words[2 * k];
                    pk.channel[k]  = words[2 * k + 1];
                }
                std::lock_guard lk(imageMutex);
                latest = std::move(img);
                latestPeaks = pk;
                ++serial;
                best = s;
            }
            ctx->Unmap(staging[s].Get(), 0);
            stagingPending[s] = false;
        }
    }
};

D3D11ScopeRenderer::D3D11ScopeRenderer() : m_impl(std::make_unique<Impl>()) {}

D3D11ScopeRenderer::~D3D11ScopeRenderer() { shutdown(); }

bool D3D11ScopeRenderer::initialize()
{
    Impl &i = *m_impl;
    if (i.accum) return true;
    i.device = static_cast<ID3D11Device *>(D3D11DeviceManager::instance().device());
    if (!i.device) return false;
    QString err;
    ComPtr<ID3DBlob> vsb = compileHlsl(kVsHlsl, "VSMain", "vs_5_0", &err);
    ComPtr<ID3DBlob> psb = compileHlsl(tapSource({}, kTapMain, {}), "PSMain", "ps_5_0", &err);
    ComPtr<ID3DBlob> pkb = compileHlsl(tapSource({}, std::string(kScopeCommon) + kPeakPs, {}),
                                       "PSMain", "ps_5_0", &err);
    ComPtr<ID3DBlob> acb = compileHlsl(std::string(kScopeCommon) + kAccumCs, "CSMain", "cs_5_0", &err);
    ComPtr<ID3DBlob> drb = compileHlsl(std::string(kScopeCommon) + kDrawCs, "CSMain", "cs_5_0", &err);
    if (!vsb || !psb || !pkb || !acb || !drb) {
        qWarning("D3D11ScopeRenderer: shader compile failed: %s", qPrintable(err));
        return false;
    }
    bool ok = SUCCEEDED(i.device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                                     nullptr, i.vs.GetAddressOf()))
           && SUCCEEDED(i.device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                                    nullptr, i.tapSignal.GetAddressOf()))
           && SUCCEEDED(i.device->CreatePixelShader(pkb->GetBufferPointer(), pkb->GetBufferSize(),
                                                    nullptr, i.peakSignal.GetAddressOf()))
           && SUCCEEDED(i.device->CreateComputeShader(acb->GetBufferPointer(), acb->GetBufferSize(),
                                                      nullptr, i.accum.GetAddressOf()))
           && SUCCEEDED(i.device->CreateComputeShader(drb->GetBufferPointer(), drb->GetBufferSize(),
                                                      nullptr, i.draw.GetAddressOf()))
           && makeCb<ScopeAccumGpu>(i.device, i.accumCb)
           && makeCb<ScopeDrawGpu>(i.device, i.drawCb);
    i.sampler = makeLinearClampSampler(i.device);
    i.lastUse.start();
    return ok && i.sampler;
}

void D3D11ScopeRenderer::shutdown()
{
    if (!m_impl) return;
    m_impl->releaseBuffers();
    m_impl->vs.Reset(); m_impl->tapSignal.Reset(); m_impl->peakSignal.Reset();
    for (auto &c : m_impl->conv) c = Impl::Conversion{};
    m_impl->accum.Reset(); m_impl->draw.Reset();
    m_impl->accumCb.Reset(); m_impl->drawCb.Reset(); m_impl->sampler.Reset();
    m_impl->device = nullptr;
}

void D3D11ScopeRenderer::setConfig(const ScopeConfig &config) { m_impl->cfg = config; }

void D3D11ScopeRenderer::setFrameStamp(quint64 stamp) { m_impl->frameStamp = stamp; }

bool D3D11ScopeRenderer::encode(void *ctxPtr, OCIOConfigManager *ocio,
                                void *srvAPtr, int wA, int hA,
                                void *srvBPtr, int wB, int hB)
{
    Impl &i = *m_impl;
    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxPtr);
    // Dual: either side can be in a timeline gap; each present side keeps
    // its own index (colour, interpretation, peaks).
    const bool hasA = srvAPtr && wA > 0 && hA > 0;
    const bool hasB = i.cfg.dual && srvBPtr && wB > 0 && hB > 0;
    if (!i.cfg.active || !ctx || (!hasA && !hasB)) return false;
    if (!i.accum || !i.ensureBuffers()) return false;
    i.lastUse.restart();
    i.collect(ctx);

    // A staging slot not waiting on the GPU; none free → skip (lag is fine).
    int slot = -1;
    for (int s = 0; s < kRingSlots; ++s) {
        if (!i.stagingPending[s]) { slot = s; break; }
    }
    if (slot < 0) return false;

    // Each side in its own interpretation (dual: B's tags can differ).
    const bool has[2] = {hasA, hasB};
    const ScopeConfig sideCfg[2] = {i.cfg.forSide(0), i.cfg.forSide(1)};
    bool converted[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        converted[s] = has[s] && sideCfg[s].tier != ScopeTier::Signal
                       && i.ensureConverted(ocio, s, sideCfg[s].colorspace, sideCfg[s].configPath);
    }
    static const std::vector<LutResource> kNoLuts;
    const bool wantPeaks = i.cfg.kind == ScopeKind::Waveform;
    const UINT zeros[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(i.grid.uav.Get(), zeros);
    ctx->ClearUnorderedAccessViewUint(i.oog.uav.Get(), zeros);
    if (wantPeaks) ctx->ClearUnorderedAccessViewUint(i.peaks.uav.Get(), zeros);

    auto tapSize = [](int w, int h, int &tw, int &th) {
        tw = std::min(w, kScopeMaxTapWidth);
        th = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * tw / w)));
    };
    // A full-screen triangle over a w × h viewport with `ps`, the source
    // and the LUTs (converted; none for Signal) at the slots `luts`
    // resolved for `ps`.
    auto drawTap = [&](ID3D11PixelShader *ps, const std::vector<LutResource> &luts,
                       ID3D11ShaderResourceView *srv, int w, int h) {
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(w);
        vp.Height = static_cast<float>(h);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->RSSetState(nullptr);
        ctx->VSSetShader(i.vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView *srvs[16] = {srv};
        ID3D11SamplerState *smps[16] = {i.sampler.Get()};
        UINT n = 1;
        for (const auto &lut : luts) {
            if (lut.texSlot >= 0 && lut.texSlot < 16) {
                srvs[lut.texSlot] = lut.srv.Get();
                n = std::max(n, static_cast<UINT>(lut.texSlot + 1));
            }
            if (lut.smpSlot >= 0 && lut.smpSlot < 16) {
                smps[lut.smpSlot] = lut.sampler.Get();
                n = std::max(n, static_cast<UINT>(lut.smpSlot + 1));
            }
        }
        ctx->PSSetShaderResources(0, n, srvs);
        ctx->PSSetSamplers(0, n, smps);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *nullSrvs[16] = {};
        ctx->PSSetShaderResources(0, n, nullSrvs);
    };
    int pixels = 0;   // draw gain: the first present side's tap size
    auto runSide = [&](ID3D11ShaderResourceView *srv, int w, int h, int side) {
        int tw = 0, th = 0;
        tapSize(w, h, tw, th);
        if (pixels == 0) pixels = tw * th;
        if (!i.ensureTap(tw, th)) return;

        // 1. Tap (+ OCIOScope) into the RGBA16F target.
        ID3D11RenderTargetView *rtv = i.tapRtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        const Impl::Conversion &c = i.conv[side];
        const bool conv = converted[side];
        drawTap(conv ? c.tap.Get() : i.tapSignal.Get(), conv ? c.luts : kNoLuts, srv, tw, th);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);

        // 2. Accumulate.
        ScopeAccumGpu u = scope_math::resolveAccum(
            sideCfg[side], conv ? c.side : InterchangeSide::None, conv, tw, th, side);
        upload(ctx, i.accumCb.Get(), u);
        ctx->CSSetShader(i.accum.Get(), nullptr, 0);
        ID3D11Buffer *cb = i.accumCb.Get();
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView *tap = i.tapSrv.Get();
        ctx->CSSetShaderResources(0, 1, &tap);
        ID3D11UnorderedAccessView *uavs[2] = {i.grid.uav.Get(), i.oog.uav.Get()};
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->Dispatch((tw + 15) / 16, (th + 15) / 16, 1);
        ID3D11ShaderResourceView *nullSrv = nullptr;
        ctx->CSSetShaderResources(0, 1, &nullSrv);
        ID3D11UnorderedAccessView *nullUavs[2] = {};
        ctx->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);

        // 3. Waveform peaks over the full source (the tap would average
        //    small highlights away): UAV only, no render target.
        if (wantPeaks) {
            u.p1[1] = static_cast<float>(w);
            u.p1[2] = static_cast<float>(h);
            u.p2[2] = 1.0f;
            upload(ctx, i.accumCb.Get(), u);
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ID3D11UnorderedAccessView *pu = i.peaks.uav.Get();
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &pu, nullptr);
            drawTap(conv ? c.peak.Get() : i.peakSignal.Get(), conv ? c.peakLuts : kNoLuts, srv, w, h);
            ID3D11UnorderedAccessView *nullUav = nullptr;
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &nullUav, nullptr);
            ID3D11Buffer *nullCb = nullptr;
            ctx->PSSetConstantBuffers(0, 1, &nullCb);
        }
    };
    if (hasA) runSide(static_cast<ID3D11ShaderResourceView *>(srvAPtr), wA, hA, 0);
    if (hasB) runSide(static_cast<ID3D11ShaderResourceView *>(srvBPtr), wB, hB, 1);

    // 4. Draw into the raw image buffer, then copy it (and the peaks) to
    //    a staging slot.
    const ScopeDrawGpu d = scope_math::resolveDraw(i.cfg, pixels);
    upload(ctx, i.drawCb.Get(), d);
    ctx->CSSetShader(i.draw.Get(), nullptr, 0);
    ID3D11Buffer *dcb = i.drawCb.Get();
    ctx->CSSetConstantBuffers(0, 1, &dcb);
    ID3D11UnorderedAccessView *duavs[3] = {i.grid.uav.Get(), i.oog.uav.Get(), i.outPx.uav.Get()};
    ctx->CSSetUnorderedAccessViews(0, 3, duavs, nullptr);
    ctx->Dispatch(kScopeGrid / 16, kScopeGrid / 16, 1);
    ID3D11UnorderedAccessView *nullUavs[3] = {};
    ctx->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopySubresourceRegion(i.staging[slot].Get(), 0, 0, 0, 0, i.outPx.buf.Get(), 0, nullptr);
    if (wantPeaks) {
        ctx->CopySubresourceRegion(i.staging[slot].Get(), 0, kScopeGrid * kScopeGrid * 4, 0, 0,
                                   i.peaks.buf.Get(), 0, nullptr);
    }
    i.stagingSides[slot] = wantPeaks ? (i.cfg.dual ? 2 : 1) : 0;
    i.stagingMeasured[slot][0] = hasA;
    i.stagingMeasured[slot][1] = hasB;
    i.stagingHdr[slot] = i.cfg.scale == ScopeScale::Hdr;
    i.stagingStamp[slot] = i.frameStamp;
    i.stagingEpoch[slot] = i.cfg.peakEpoch;
    i.stagingPending[slot] = true;
    i.stagingOrder[slot]   = ++i.submitCount;
    return true;
}

void D3D11ScopeRenderer::collectPending(void *ctxPtr)
{
    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxPtr);
    if (ctx && m_impl->grid.buf) m_impl->collect(ctx);
}

bool D3D11ScopeRenderer::latestImage(QImage *out, quint64 *serial, ScopePeaks *peaks) const
{
    std::lock_guard lk(m_impl->imageMutex);
    if (m_impl->latest.isNull()) return false;
    if (out) *out = m_impl->latest;
    if (peaks) *peaks = m_impl->latestPeaks;
    if (serial) *serial = m_impl->serial;
    return true;
}

void D3D11ScopeRenderer::releaseIfIdle(int idleMs)
{
    if (m_impl->grid.buf && m_impl->lastUse.elapsed() > idleMs) {
        m_impl->releaseBuffers();
        std::lock_guard lk(m_impl->imageMutex);
        m_impl->latest = QImage();
        qInfo("D3D11ScopeRenderer: idle — buffers released");
    }
}

} // namespace qcv
