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

// Shared math for the compute kernels. Mirrors scope_math::bin().
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
    float3 rgb = c.rgb;
    float3 ycc;
    bool outside = false;
    float nits = 0.0;
    if (p0.x > 0.5) {
        float3 l = float3(dot(to0.xyz, rgb), dot(to1.xyz, rgb), dot(to2.xyz, rgb));
        if (p0.y < 0.5) {
            float3 l7 = float3( 1.6604910 * l.r - 0.5876411 * l.g - 0.0728499 * l.b,
                               -0.1245505 * l.r + 1.1328999 * l.g - 0.0083494 * l.b,
                               -0.0181508 * l.r - 0.1005789 * l.g + 1.1187297 * l.b);
            outside = any(l7 < -0.002);
            float3 e = float3(qs_spow(l7.r, 1.0 / 2.4), qs_spow(l7.g, 1.0 / 2.4),
                              qs_spow(l7.b, 1.0 / 2.4));
            ycc = qs_ycc(e, 1);
        } else {
            outside = any(l < -0.002);
            nits = 100.0 * dot(float3(0.2627, 0.6780, 0.0593), l);
            float3 e = sign(l) * float3(qs_pq(abs(l.r) * 0.01), qs_pq(abs(l.g) * 0.01),
                                        qs_pq(abs(l.b) * 0.01));
            ycc = qs_ycc(e, 2);
        }
    } else {
        float3 e = rgb;
        if (p1.x > 0.5) {
            e = float3(qs_spow(e.r, 1.0 / 2.4), qs_spow(e.g, 1.0 / 2.4), qs_spow(e.b, 1.0 / 2.4));
        }
        ycc = qs_ycc(e, (int)p0.w);
    }
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

constexpr const char *kDrawCs = R"(
RWByteAddressBuffer grid    : register(u0);
RWByteAddressBuffer oog     : register(u1);
RWByteAddressBuffer persist : register(u2);
RWByteAddressBuffer outPx   : register(u3);
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
        uint pi = ((uint)s * (uint)(G * G) + cell) * 4;
        if (d0.z > 0.0) I = max(I, asfloat(persist.Load(pi)) * d0.z);
        persist.Store(pi, asuint(I));
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
    ComPtr<ID3D11ComputeShader> accum;
    ComPtr<ID3D11ComputeShader> draw;
    ComPtr<ID3D11SamplerState>  sampler;
    ComPtr<ID3D11Buffer>        accumCb, drawCb;

    // Converted tap PS (OCIOScope inside), keyed on config + colourspace.
    ComPtr<ID3D11PixelShader> tapConverted;
    std::vector<LutResource>  luts;
    QString          convertedKey;
    InterchangeSide  convertedSide = InterchangeSide::None;

    // Tap target (reallocated on size change).
    ComPtr<ID3D11Texture2D>          tapTex;
    ComPtr<ID3D11RenderTargetView>   tapRtv;
    ComPtr<ID3D11ShaderResourceView> tapSrv;
    int tapW = 0, tapH = 0;

    RawBuffer grid, oog, persist, outPx;
    ComPtr<ID3D11Buffer> staging[kRingSlots];
    bool     stagingPending[kRingSlots] = {false, false, false};
    quint64  stagingOrder[kRingSlots]   = {0, 0, 0};
    quint64  submitCount = 0;

    mutable std::mutex imageMutex;
    QImage   latest;
    quint64  serial = 0;

    ScopeConfig   cfg;
    QElapsedTimer lastUse;

    bool ensureBuffers()
    {
        if (grid.buf) return true;
        const UINT g2 = kScopeGrid * kScopeGrid;
        bool ok = grid.create(device, 2 * kScopeCopies * g2 * 4)
               && oog.create(device, 2 * g2 * 4)
               && persist.create(device, 2 * g2 * 4)
               && outPx.create(device, g2 * 4);
        for (auto &s : staging) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = g2 * 4;
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
        grid.reset(); oog.reset(); persist.reset(); outPx.reset();
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

    bool ensureConverted(OCIOConfigManager *ocio)
    {
        if (!ocio || cfg.colorspace.isEmpty()) return false;
        const QString key = ocio->configIdentifier() + QLatin1Char('|') + cfg.colorspace;
        if (key == convertedKey) return static_cast<bool>(tapConverted);
        convertedKey = key;
        tapConverted.Reset();
        luts.clear();
        InterchangeSide side = InterchangeSide::None;
        OcioChain chain = OcioChainBuilder::buildScope(
            ocio, OcioChainBuilder::Language::Hlsl_Sm_5_0, cfg.colorspace, &side);
        if (!chain.ok) {
            qInfo("D3D11ScopeRenderer: no conversion for '%s' (%s) — Signal",
                  qPrintable(cfg.colorspace), qPrintable(chain.errorMessage));
            return false;
        }
        QString err;
        if (!createLuts(device, chain.desc, luts, err)) { luts.clear(); return false; }
        std::string src = kTapHead;
        src += chain.shaderText.toStdString();
        std::string main = kTapMain;
        const std::string marker = "QCV_SCOPE_CONVERT";
        main.replace(main.find(marker), marker.size(), "rgb = OCIOScope(float4(rgb, 1.0)).rgb;");
        src += main;
        ComPtr<ID3DBlob> blob = compileHlsl(src, "PSMain", "ps_5_0", &err);
        if (!blob || FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                      nullptr, tapConverted.GetAddressOf()))) {
            qWarning("D3D11ScopeRenderer: tap compile failed: %s", qPrintable(err));
            tapConverted.Reset();
            luts.clear();
            return false;
        }
        int byName = 0, byFallback = 0;
        resolveLutSlots(blob.Get(), luts, byName, byFallback);
        convertedSide = side;
        qInfo("D3D11ScopeRenderer: conversion built for '%s' (%zu LUTs)",
              qPrintable(cfg.colorspace), luts.size());
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
                QImage img(kScopeGrid, kScopeGrid, QImage::Format_RGBA8888_Premultiplied);
                std::memcpy(img.bits(), m.pData, static_cast<size_t>(kScopeGrid) * kScopeGrid * 4);
                std::lock_guard lk(imageMutex);
                latest = std::move(img);
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
    std::string tapSrc = std::string(kTapHead) + kTapMain;
    tapSrc.replace(tapSrc.find("QCV_SCOPE_CONVERT"), std::strlen("QCV_SCOPE_CONVERT"), "");
    ComPtr<ID3DBlob> psb = compileHlsl(tapSrc, "PSMain", "ps_5_0", &err);
    ComPtr<ID3DBlob> acb = compileHlsl(std::string(kScopeCommon) + kAccumCs, "CSMain", "cs_5_0", &err);
    ComPtr<ID3DBlob> drb = compileHlsl(std::string(kScopeCommon) + kDrawCs, "CSMain", "cs_5_0", &err);
    if (!vsb || !psb || !acb || !drb) {
        qWarning("D3D11ScopeRenderer: shader compile failed: %s", qPrintable(err));
        return false;
    }
    bool ok = SUCCEEDED(i.device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                                     nullptr, i.vs.GetAddressOf()))
           && SUCCEEDED(i.device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                                    nullptr, i.tapSignal.GetAddressOf()))
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
    m_impl->vs.Reset(); m_impl->tapSignal.Reset(); m_impl->tapConverted.Reset();
    m_impl->accum.Reset(); m_impl->draw.Reset();
    m_impl->accumCb.Reset(); m_impl->drawCb.Reset(); m_impl->sampler.Reset();
    m_impl->luts.clear();
    m_impl->convertedKey.clear();
    m_impl->device = nullptr;
}

void D3D11ScopeRenderer::setConfig(const ScopeConfig &config) { m_impl->cfg = config; }

bool D3D11ScopeRenderer::encode(void *ctxPtr, OCIOConfigManager *ocio,
                                void *srvAPtr, int wA, int hA,
                                void *srvBPtr, int wB, int hB)
{
    Impl &i = *m_impl;
    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxPtr);
    if (!i.cfg.active || !ctx || !srvAPtr || wA <= 0 || hA <= 0) return false;
    if (!i.accum || !i.ensureBuffers()) return false;
    i.lastUse.restart();
    i.collect(ctx);

    // A staging slot not waiting on the GPU; none free → skip (lag is fine).
    int slot = -1;
    for (int s = 0; s < kRingSlots; ++s) {
        if (!i.stagingPending[s]) { slot = s; break; }
    }
    if (slot < 0) return false;

    const bool converted = i.cfg.tier != ScopeTier::Signal && i.ensureConverted(ocio);
    const UINT zeros[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(i.grid.uav.Get(), zeros);
    ctx->ClearUnorderedAccessViewUint(i.oog.uav.Get(), zeros);

    auto tapSize = [](int w, int h, int &tw, int &th) {
        tw = std::min(w, kScopeMaxTapWidth);
        th = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * tw / w)));
    };
    int pixelsA = 0;
    auto runSide = [&](ID3D11ShaderResourceView *srv, int w, int h, int side) {
        int tw = 0, th = 0;
        tapSize(w, h, tw, th);
        if (side == 0) pixelsA = tw * th;
        if (!i.ensureTap(tw, th)) return;

        // 1. Tap (+ OCIOScope) into the RGBA16F target.
        ID3D11RenderTargetView *rtv = i.tapRtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(tw);
        vp.Height = static_cast<float>(th);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->RSSetState(nullptr);
        ctx->VSSetShader(i.vs.Get(), nullptr, 0);
        ctx->PSSetShader(converted ? i.tapConverted.Get() : i.tapSignal.Get(), nullptr, 0);
        ID3D11ShaderResourceView *srvs[16] = {srv};
        ID3D11SamplerState *smps[16] = {i.sampler.Get()};
        UINT n = 1;
        if (converted) {
            for (const auto &lut : i.luts) {
                if (lut.texSlot >= 0 && lut.texSlot < 16) {
                    srvs[lut.texSlot] = lut.srv.Get();
                    n = std::max(n, static_cast<UINT>(lut.texSlot + 1));
                }
                if (lut.smpSlot >= 0 && lut.smpSlot < 16) {
                    smps[lut.smpSlot] = lut.sampler.Get();
                    n = std::max(n, static_cast<UINT>(lut.smpSlot + 1));
                }
            }
        }
        ctx->PSSetShaderResources(0, n, srvs);
        ctx->PSSetSamplers(0, n, smps);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *nullSrvs[16] = {};
        ctx->PSSetShaderResources(0, n, nullSrvs);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);

        // 2. Accumulate.
        const ScopeAccumGpu u = scope_math::resolveAccum(
            i.cfg, converted ? i.convertedSide : InterchangeSide::None, converted, tw, th, side);
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
    };
    runSide(static_cast<ID3D11ShaderResourceView *>(srvAPtr), wA, hA, 0);
    const bool dual = i.cfg.dual && srvBPtr && wB > 0 && hB > 0;
    if (dual) runSide(static_cast<ID3D11ShaderResourceView *>(srvBPtr), wB, hB, 1);

    // 3. Draw into the raw image buffer, then copy to a staging slot.
    const ScopeDrawGpu d = scope_math::resolveDraw(i.cfg, pixelsA);
    upload(ctx, i.drawCb.Get(), d);
    ctx->CSSetShader(i.draw.Get(), nullptr, 0);
    ID3D11Buffer *dcb = i.drawCb.Get();
    ctx->CSSetConstantBuffers(0, 1, &dcb);
    ID3D11UnorderedAccessView *duavs[4] = {i.grid.uav.Get(), i.oog.uav.Get(),
                                           i.persist.uav.Get(), i.outPx.uav.Get()};
    ctx->CSSetUnorderedAccessViews(0, 4, duavs, nullptr);
    ctx->Dispatch(kScopeGrid / 16, kScopeGrid / 16, 1);
    ID3D11UnorderedAccessView *nullUavs[4] = {};
    ctx->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(i.staging[slot].Get(), i.outPx.buf.Get());
    i.stagingPending[slot] = true;
    i.stagingOrder[slot]   = ++i.submitCount;
    return true;
}

void D3D11ScopeRenderer::collectPending(void *ctxPtr)
{
    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxPtr);
    if (ctx && m_impl->grid.buf) m_impl->collect(ctx);
}

bool D3D11ScopeRenderer::latestImage(QImage *out, quint64 *serial) const
{
    std::lock_guard lk(m_impl->imageMutex);
    if (m_impl->latest.isNull()) return false;
    if (out) *out = m_impl->latest;
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
