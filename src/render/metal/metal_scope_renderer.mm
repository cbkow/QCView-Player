// MetalScopeRenderer — see header.

#include "metal_scope_renderer.h"

#include "color/ocio_chain_builder.h"
#include "color/ocio_config_manager.h"
#include "metal_device_manager.h"
#include "metal_ocio_luts.h"

#import <Metal/Metal.h>

#include <QElapsedTimer>
#include <QtLogging>

#include <atomic>
#include <cstring>
#include <mutex>

namespace qcv {

namespace {

constexpr int kRingSlots = 3;

constexpr const char *kScopeHeader = R"(
#include <metal_stdlib>
using namespace metal;

struct ScopeAccum { float4 to0, to1, to2; float4 p0; float4 p1; float4 p2; };
struct ScopeDraw  { float4 p0; float4 p1; };

constant int G = 512;
constant int COPIES = 4;

static float qs_spow(float x, float k) { return sign(x) * pow(abs(x), k); }

static float qs_pq(float y)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(max(y, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// (Y′, Cb, Cr) for matrix 0 = BT.601, 1 = BT.709, 2 = BT.2020.
static float3 qs_ycc(float3 e, int m)
{
    float kr = 0.2126, kb = 0.0722;
    if (m == 2)      { kr = 0.2627; kb = 0.0593; }
    else if (m == 0) { kr = 0.299;  kb = 0.114;  }
    float y = kr * e.r + (1.0 - kr - kb) * e.g + kb * e.b;
    return float3(y, (e.b - y) / (2.0 * (1.0 - kb)), (e.r - y) / (2.0 * (1.0 - kr)));
}
)";

// Accumulate kernel, split so the converted variant can splice the OCIO
// function's arguments and call in. Mirrors scope_math::bin().
constexpr const char *kAccumPrefix = R"(
kernel void scope_accum(
    texture2d<float, access::sample> src     [[texture(0)]],
    sampler                          src_smp [[sampler(0)]],
)";

constexpr const char *kAccumBodyHead = R"(
    constant ScopeAccum &u          [[buffer(0)]],
    device atomic_uint  *grid       [[buffer(1)]],
    device atomic_uint  *oog        [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]],
    uint2 tg  [[threadgroup_position_in_grid]])
{
    uint w = uint(u.p1.y), h = uint(u.p1.z);
    if (gid.x >= w || gid.y >= h) return;
    float2 uv = (float2(gid) + 0.5) / float2(w, h);
    float4 c = src.sample(src_smp, uv, level(0.0));
    if (c.a <= 0.0) return;
    float3 rgb = c.rgb;
    float3 ycc;
    bool outside = false;
    float nits = 0.0;
    if (u.p0.x > 0.5) {
)";

constexpr const char *kAccumBodyTail = R"(
        float3 l = float3(dot(u.to0.xyz, rgb), dot(u.to1.xyz, rgb), dot(u.to2.xyz, rgb));
        if (u.p0.y < 0.5) {
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
        if (u.p1.x > 0.5) {
            e = float3(qs_spow(e.r, 1.0 / 2.4), qs_spow(e.g, 1.0 / 2.4), qs_spow(e.b, 1.0 / 2.4));
        }
        ycc = qs_ycc(e, int(u.p0.w));
    }
    int bx, by;
    if (u.p2.x > 0.5) {
        // Waveform: column × Y′ / nits (scope_math::bin / waveformRange).
        bool hdr = u.p0.x > 0.5 && u.p0.y > 0.5;
        float lo = hdr ? 0.0 : -0.1, hi = hdr ? u.p2.y : 1.1;
        if (hdr) ycc.x = nits;
        outside = hdr ? ycc.x > hi + 1e-3 : (ycc.x > 1.0 + 1e-4 || ycc.x < -1e-4);
        bx = clamp(int(floor((float(gid.x) + 0.5) / float(w) * float(G))), 0, G - 1);
        by = clamp(int(floor((hi - ycc.x) / (hi - lo) * float(G))), 0, G - 1);
    } else {
        float zoom = u.p0.z;
        bx = clamp(int(floor((ycc.y * zoom + 0.5) * float(G))), 0, G - 1);
        by = clamp(int(floor((0.5 - ycc.z * zoom) * float(G))), 0, G - 1);
    }
    uint side = uint(u.p1.w);
    uint copy = (tg.x + tg.y) % uint(COPIES);
    uint cell = uint(by * G + bx);
    atomic_fetch_add_explicit(&grid[(side * uint(COPIES) + copy) * uint(G * G) + cell], 1u,
                              memory_order_relaxed);
    if (outside) {
        atomic_fetch_add_explicit(&oog[side * uint(G * G) + cell], 1u, memory_order_relaxed);
    }
}
)";

constexpr const char *kDrawKernel = R"(
kernel void scope_draw(
    device const uint *grid    [[buffer(0)]],
    device const uint *oog     [[buffer(1)]],
    device float      *persist [[buffer(2)]],
    device uint       *outPx   [[buffer(3)]],
    constant ScopeDraw &u      [[buffer(4)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= uint(G) || gid.y >= uint(G)) return;
    uint cell = gid.y * uint(G) + gid.x;
    int sides = int(u.p0.w);
    float3 col = float3(0.0);
    float alpha = 0.0;
    for (int s = 0; s < sides; ++s) {
        uint c = 0u;
        for (int k = 0; k < COPIES; ++k) c += grid[uint(s * COPIES + k) * uint(G * G) + cell];
        uint o = oog[uint(s) * uint(G * G) + cell];
        // Any occupied bin shows at least faintly (a handful of specular
        // pixels must not vanish), then exposure-style build-up.
        float I = c > 0u ? max(1.0 - exp(-u.p0.x * float(c)), 0.22) : 0.0;
        // 3×3 glow: flat colours (bars) land in a single bin — a lone
        // pixel at 512² is hard to see — so a bin also shows 60 % of its
        // brightest neighbour.
        float halo = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int x = int(gid.x) + dx, y = int(gid.y) + dy;
                if ((dx == 0 && dy == 0) || x < 0 || y < 0 || x >= G || y >= G) continue;
                uint n = 0u;
                uint nc = uint(y * G + x);
                for (int k = 0; k < COPIES; ++k) n += grid[uint(s * COPIES + k) * uint(G * G) + nc];
                halo = max(halo, n > 0u ? max(1.0 - exp(-u.p0.x * float(n)), 0.22) : 0.0);
            }
        }
        I = max(I, 0.6 * halo);
        uint pi = uint(s) * uint(G * G) + cell;
        if (u.p0.z > 0.0) I = max(I, persist[pi] * u.p0.z);
        persist[pi] = I;
        float3 tint;
        if (sides == 2) {
            tint = s == 0 ? float3(0.30, 0.85, 1.0) : float3(1.0, 0.62, 0.25);
        } else if (u.p0.y > 0.5 && u.p1.y < 0.5) {   // colourise: vectorscope only
            // Colourised by bin position (Rec.709 at mid grey) — a hue
            // cue only; exact only where the colour space is known.
            float zoom = u.p1.x;
            float cb = ((float(gid.x) + 0.5) / float(G) - 0.5) / zoom;
            float cr = (0.5 - (float(gid.y) + 0.5) / float(G)) / zoom;
            float3 rgb = clamp(float3(0.5 + 1.5748 * cr,
                                      0.5 - 0.1873 * cb - 0.4681 * cr,
                                      0.5 + 1.8556 * cb), 0.0, 1.0);
            float m = max(rgb.r, max(rgb.g, rgb.b));
            tint = mix(float3(1.0), rgb / max(m, 1e-3), 0.85);
        } else {
            tint = float3(0.82, 0.95, 0.82);
        }
        if (c > 0u && o * 2u > c) tint = float3(1.0, 0.28, 0.36);   // mostly out of gamut
        col += tint * I;
        alpha = max(alpha, I);
    }
    col = min(col, float3(alpha));                                   // premultiplied
    uint4 q = uint4(round(saturate(float4(col, alpha)) * 255.0));
    outPx[cell] = q.r | (q.g << 8) | (q.b << 16) | (q.a << 24);
}
)";

id<MTLComputePipelineState> compile(id<MTLDevice> device, const QString &src,
                                    NSString *fn, QString &error)
{
    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:
                                                          src.toUtf8().constData()]
                                              options:nil error:&err];
    if (!lib) {
        error = QStringLiteral("MetalScopeRenderer: compile failed: %1")
                    .arg(err ? QString::fromUtf8(err.localizedDescription.UTF8String) : QString());
        qWarning("%s", qPrintable(error));
        return nil;
    }
    id<MTLFunction> f = [lib newFunctionWithName:fn];
    id<MTLComputePipelineState> pso =
        f ? [device newComputePipelineStateWithFunction:f error:&err] : nil;
    if (!pso) {
        error = QStringLiteral("MetalScopeRenderer: pipeline failed");
        qWarning("%s", qPrintable(error));
    }
    return pso;
}

} // namespace

struct MetalScopeRenderer::Impl {
    id<MTLDevice>               device  = nil;
    id<MTLSamplerState>         sampler = nil;
    id<MTLComputePipelineState> accumSignal = nil;
    id<MTLComputePipelineState> draw        = nil;

    // Converted variant (OCIOScope inside), keyed on config + colourspace.
    id<MTLComputePipelineState> accumConverted = nil;
    std::vector<id<MTLTexture>> luts;
    QString          convertedKey;
    InterchangeSide  convertedSide = InterchangeSide::None;
    bool             convertedFailed = false;

    // Buffers (lazy; ~15 MB).
    id<MTLBuffer> grid    = nil;   // 2 sides × COPIES × G² uint
    id<MTLBuffer> oog     = nil;   // 2 sides × G² uint
    id<MTLBuffer> persist = nil;   // 2 sides × G² float
    id<MTLBuffer> ring[kRingSlots] = {nil, nil, nil};

    mutable std::mutex ringMutex;
    int  latest = -1;
    mutable int reading = -1;
    bool inFlight[kRingSlots] = {false, false, false};
    std::atomic<quint64> serial{0};

    ScopeConfig    cfg;
    QElapsedTimer  lastUse;

    bool ensureBuffers()
    {
        if (grid) return true;
        const NSUInteger g2 = static_cast<NSUInteger>(kScopeGrid) * kScopeGrid;
        grid    = [device newBufferWithLength:2 * kScopeCopies * g2 * 4 options:MTLResourceStorageModePrivate];
        oog     = [device newBufferWithLength:2 * g2 * 4 options:MTLResourceStorageModePrivate];
        persist = [device newBufferWithLength:2 * g2 * 4 options:MTLResourceStorageModePrivate];
        for (auto &r : ring) r = [device newBufferWithLength:g2 * 4 options:MTLResourceStorageModeShared];
        if (!grid || !oog || !persist || !ring[0] || !ring[1] || !ring[2]) {
            releaseBuffers();
            return false;
        }
        qInfo("MetalScopeRenderer: buffers allocated");
        return true;
    }

    void releaseBuffers()
    {
        std::lock_guard lk(ringMutex);
        grid = nil; oog = nil; persist = nil;
        for (auto &r : ring) r = nil;
        latest = -1;
        for (bool &f : inFlight) f = false;
    }

    // Build (or reuse) the converted accumulate pipeline for the config's
    // colourspace. False = unavailable (data colourspace, no role, compile
    // error) — the caller falls back to Signal.
    bool ensureConverted(OCIOConfigManager *ocio)
    {
        if (!ocio || cfg.colorspace.isEmpty()) return false;
        const QString key = ocio->configIdentifier() + QLatin1Char('|') + cfg.colorspace;
        if (key == convertedKey) return accumConverted != nil;
        convertedKey = key;
        accumConverted = nil;
        luts.clear();
        InterchangeSide side = InterchangeSide::None;
        OcioChain chain = OcioChainBuilder::buildScope(
            ocio, OcioChainBuilder::Language::Msl_2_0, cfg.colorspace, &side);
        if (!chain.ok) {
            qInfo("MetalScopeRenderer: no conversion for '%s' (%s) — Signal",
                  qPrintable(cfg.colorspace), qPrintable(chain.errorMessage));
            return false;
        }
        int nextTex = 1;   // 0 = src
        QString args, call, err;
        if (!gatherLuts(device, chain.desc, nextTex, luts, args, call, err)) return false;
        const QString src =
            QString::fromUtf8(kScopeHeader) + chain.shaderText
            + QString::fromUtf8(kAccumPrefix) + args + QString::fromUtf8(kAccumBodyHead)
            + QStringLiteral("        rgb = OCIOScope(%1float4(rgb, 1.0)).rgb;\n").arg(call)
            + QString::fromUtf8(kAccumBodyTail);
        accumConverted = compile(device, src, @"scope_accum", err);
        if (!accumConverted) { luts.clear(); return false; }
        convertedSide = side;
        qInfo("MetalScopeRenderer: conversion built for '%s' (%zu LUTs)",
              qPrintable(cfg.colorspace), luts.size());
        return true;
    }
};

MetalScopeRenderer::MetalScopeRenderer() : m_impl(std::make_unique<Impl>()) {}

MetalScopeRenderer::~MetalScopeRenderer() { shutdown(); }

bool MetalScopeRenderer::initialize()
{
    Impl &i = *m_impl;
    if (i.accumSignal) return true;
    i.device = (__bridge id<MTLDevice>)MetalDeviceManager::instance().device();
    if (!i.device) return false;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    i.sampler = [i.device newSamplerStateWithDescriptor:sd];
    QString err;
    i.accumSignal = compile(i.device,
                            QString::fromUtf8(kScopeHeader) + QString::fromUtf8(kAccumPrefix)
                                + QString::fromUtf8(kAccumBodyHead)
                                + QString::fromUtf8(kAccumBodyTail),
                            @"scope_accum", err);
    i.draw = compile(i.device, QString::fromUtf8(kScopeHeader) + QString::fromUtf8(kDrawKernel),
                     @"scope_draw", err);
    i.lastUse.start();
    return i.accumSignal && i.draw && i.sampler;
}

void MetalScopeRenderer::shutdown()
{
    if (!m_impl) return;
    m_impl->releaseBuffers();
    m_impl->accumSignal = nil;
    m_impl->accumConverted = nil;
    m_impl->draw = nil;
    m_impl->luts.clear();
    m_impl->convertedKey.clear();
    m_impl->device = nil;
}

void MetalScopeRenderer::setConfig(const ScopeConfig &config) { m_impl->cfg = config; }

const ScopeConfig &MetalScopeRenderer::config() const { return m_impl->cfg; }

quint64 MetalScopeRenderer::serial() const { return m_impl->serial.load(); }

bool MetalScopeRenderer::encode(void *cmdBufPtr, OCIOConfigManager *ocio,
                                void *srcAPtr, int wA, int hA,
                                void *srcBPtr, int wB, int hB)
{
    Impl &i = *m_impl;
    if (!i.cfg.active || !cmdBufPtr || !srcAPtr || wA <= 0 || hA <= 0) return false;
    if (!i.accumSignal || !i.draw || !i.ensureBuffers()) return false;
    i.lastUse.restart();

    // A free readback slot: not the newest (the GUI may want it), not the
    // one being copied, not still on the GPU. None free → skip (lag is fine).
    int slot = -1;
    {
        std::lock_guard lk(i.ringMutex);
        for (int s = 0; s < kRingSlots; ++s) {
            if (s != i.latest && s != i.reading && !i.inFlight[s]) { slot = s; break; }
        }
        if (slot < 0) return false;
        i.inFlight[slot] = true;
    }

    const bool converted = i.cfg.tier != ScopeTier::Signal && i.ensureConverted(ocio);
    id<MTLComputePipelineState> accum = converted ? i.accumConverted : i.accumSignal;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmdBufPtr;

    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:i.grid range:NSMakeRange(0, i.grid.length) value:0];
    [blit fillBuffer:i.oog  range:NSMakeRange(0, i.oog.length)  value:0];
    [blit endEncoding];

    auto tapSize = [](int w, int h, int &tw, int &th) {
        tw = std::min(w, kScopeMaxTapWidth);
        th = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * tw / w)));
    };
    int tapWA = 0, tapHA = 0;
    tapSize(wA, hA, tapWA, tapHA);
    if (qEnvironmentVariableIsSet("QCV_SCOPE_DEBUG")) {
        static int n = 0;
        if (n++ < 3) {
            id<MTLTexture> t = (__bridge id<MTLTexture>)srcAPtr;
            qInfo("MetalScopeRenderer: tap A %dx%d (texture %lux%lu fmt %lu) → %dx%d, tier %d, "
                  "cs '%s', scale %d, zoom %d",
                  wA, hA, (unsigned long)t.width, (unsigned long)t.height,
                  (unsigned long)t.pixelFormat, tapWA, tapHA, static_cast<int>(i.cfg.tier),
                  qPrintable(i.cfg.colorspace), static_cast<int>(i.cfg.scale), i.cfg.zoom);
        }
    }

    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:accum];
    [enc setSamplerState:i.sampler atIndex:0];
    if (converted) {
        int t = 1;
        for (id<MTLTexture> lut : i.luts) [enc setTexture:lut atIndex:t++];
    }
    [enc setBuffer:i.grid offset:0 atIndex:1];
    [enc setBuffer:i.oog  offset:0 atIndex:2];
    auto dispatchSide = [&](void *srcPtr, int w, int h, int side) {
        int tw = 0, th = 0;
        tapSize(w, h, tw, th);
        const ScopeAccumGpu u = scope_math::resolveAccum(
            i.cfg, converted ? i.convertedSide : InterchangeSide::None, converted, tw, th, side);
        [enc setTexture:(__bridge id<MTLTexture>)srcPtr atIndex:0];
        [enc setBytes:&u length:sizeof(u) atIndex:0];
        [enc dispatchThreads:MTLSizeMake(tw, th, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    };
    dispatchSide(srcAPtr, wA, hA, 0);
    const bool dual = i.cfg.dual && srcBPtr && wB > 0 && hB > 0;
    if (dual) dispatchSide(srcBPtr, wB, hB, 1);

    ScopeConfig drawCfg = i.cfg;
    drawCfg.dual = i.cfg.dual;
    const ScopeDrawGpu d = scope_math::resolveDraw(drawCfg, tapWA * tapHA);
    [enc setComputePipelineState:i.draw];
    [enc setBuffer:i.grid       offset:0 atIndex:0];
    [enc setBuffer:i.oog        offset:0 atIndex:1];
    [enc setBuffer:i.persist    offset:0 atIndex:2];
    [enc setBuffer:i.ring[slot] offset:0 atIndex:3];
    [enc setBytes:&d length:sizeof(d) atIndex:4];
    [enc dispatchThreads:MTLSizeMake(kScopeGrid, kScopeGrid, 1)
        threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];

    Impl *impl = m_impl.get();
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        std::lock_guard lk(impl->ringMutex);
        impl->inFlight[slot] = false;
        if (impl->grid) {           // not released meanwhile
            impl->latest = slot;
            impl->serial.fetch_add(1);
        }
    }];
    return true;
}

bool MetalScopeRenderer::latestImage(QImage *out, quint64 *serialOut) const
{
    Impl &i = *m_impl;
    int slot;
    id<MTLBuffer> buf;
    {
        std::lock_guard lk(i.ringMutex);
        if (i.latest < 0 || !i.ring[i.latest]) return false;
        slot = i.latest;
        buf = i.ring[slot];
        i.reading = slot;
        if (serialOut) *serialOut = i.serial.load();
    }
    QImage img(kScopeGrid, kScopeGrid, QImage::Format_RGBA8888_Premultiplied);
    std::memcpy(img.bits(), buf.contents, static_cast<size_t>(kScopeGrid) * kScopeGrid * 4);
    {
        std::lock_guard lk(i.ringMutex);
        i.reading = -1;
    }
    if (out) *out = std::move(img);
    return true;
}

void MetalScopeRenderer::releaseIfIdle(int idleMs)
{
    if (m_impl->grid && m_impl->lastUse.elapsed() > idleMs) {
        m_impl->releaseBuffers();
        qInfo("MetalScopeRenderer: idle — buffers released");
    }
}

bool MetalScopeRenderer::readCounts(std::vector<uint32_t> &counts, std::vector<uint32_t> &oogOut) const
{
    Impl &i = *m_impl;
    if (!i.grid) return false;
    const size_t g2 = static_cast<size_t>(kScopeGrid) * kScopeGrid;
    id<MTLBuffer> tmpG = [i.device newBufferWithLength:kScopeCopies * g2 * 4
                                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> tmpO = [i.device newBufferWithLength:g2 * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [i.device newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:i.grid sourceOffset:0 toBuffer:tmpG destinationOffset:0 size:tmpG.length];
    [blit copyFromBuffer:i.oog sourceOffset:0 toBuffer:tmpO destinationOffset:0 size:tmpO.length];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const auto *g = static_cast<const uint32_t *>(tmpG.contents);
    const auto *o = static_cast<const uint32_t *>(tmpO.contents);
    counts.assign(g2, 0);
    for (int k = 0; k < kScopeCopies; ++k) {
        for (size_t c = 0; c < g2; ++c) counts[c] += g[k * g2 + c];
    }
    oogOut.assign(o, o + g2);
    return true;
}

} // namespace qcv
