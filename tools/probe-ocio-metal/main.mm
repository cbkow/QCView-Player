// probe-ocio-metal — GPU check of MetalOcioRenderer's split chain.
//
// Runs test values through the real MetalOcioRenderer (unsplit, split
// with gain, split with the knee) on a 1-row RGBA16F texture and compares
// the result with the CPU reference (OCIO CPU processors + the C++
// linear stage). Proves the composed MSL compiles and that GPU and CPU
// agree. macOS only; opens a hidden Metal window for the device.
//
// Usage:  probe-ocio-metal <assets/OCIO dir>

#include "color/linear_stage.h"
#include "color/ocio_chain_builder.h"
#include "color/ocio_config_manager.h"
#include "render/metal/metal_device_manager.h"
#include "render/metal/metal_ocio_renderer.h"
#include "render/metal/metal_scope_renderer.h"
#include "color/scope_math.h"
#include "color/scope_names.h"

#include <QGuiApplication>
#include <QPainter>
#include <QWindow>

#import <Metal/Metal.h>

#include <OpenColorIO/OpenColorIO.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;
using namespace qcv;

namespace {

struct Case { const char *config, *input, *display, *view; };

const std::vector<std::array<float, 3>> kValues = {
    {0.0f, 0.0f, 0.0f}, {0.05f, 0.05f, 0.05f}, {0.18f, 0.18f, 0.18f},
    {0.5f, 0.5f, 0.5f}, {0.75f, 0.4f, 0.2f}, {0.2f, 0.6f, 0.9f},
    {0.9f, 0.9f, 0.9f}, {1.0f, 1.0f, 1.0f},
};

// CPU reference: unsplit chain for an identity stage, else split + stage.
void cpuRef(OCIOConfigManager &mgr, OCIO::ConstConfigRcPtr cfg,
            const LinearStageSettings &st, const ViewerAids &va,
            std::vector<std::array<float, 3>> &out)
{
    out = kValues;
    auto viewer = [&](OutputEncoding enc, int prim) {
        const ViewerGpu g = linear_stage::resolveViewer(va, enc, prim);
        for (auto &v : out) {
            float rgba[4] = {v[0], v[1], v[2], 1.0f};
            linear_stage::applyViewer(rgba, g);
            v = {rgba[0], rgba[1], rgba[2]};
        }
    };
    if (linear_stage::isIdentity(st)) {
        auto cpu = cfg->getProcessor(OcioChainBuilder::buildGroupTransform(&mgr, cfg))
                       ->getDefaultCPUProcessor();
        for (auto &v : out) cpu->applyRGB(v.data());
        const OcioChain c = OcioChainBuilder::build(&mgr, OcioChainBuilder::Language::Msl_2_0);
        viewer(c.encoding, c.outputPrimaries);
        return;
    }
    OcioSplitTransforms t;
    OcioChainBuilder::buildSplitTransforms(&mgr, cfg, t);
    auto pre  = cfg->getProcessor(t.pre)->getDefaultCPUProcessor();
    auto post = cfg->getProcessor(t.post)->getDefaultCPUProcessor();
    const LinearStageGpu g = linear_stage::resolve(st, t.side, t.displayIsSdr);
    for (auto &v : out) {
        pre->applyRGB(v.data());
        linear_stage::apply(v.data(), g);
        post->applyRGB(v.data());
    }
    viewer(t.encoding, t.outputPrimaries);
}

bool gpuRun(id<MTLDevice> dev, id<MTLCommandQueue> q, MetalOcioRenderer &r,
            std::vector<std::array<float, 3>> &out)
{
    const int W = static_cast<int>(kValues.size());
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:1 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> src = [dev newTextureWithDescriptor:td];
    std::vector<float> px(W * 4);
    for (int i = 0; i < W; ++i) {
        px[i * 4 + 0] = kValues[i][0]; px[i * 4 + 1] = kValues[i][1];
        px[i * 4 + 2] = kValues[i][2]; px[i * 4 + 3] = 1.0f;
    }
    [src replaceRegion:MTLRegionMake2D(0, 0, W, 1) mipmapLevel:0 withBytes:px.data()
           bytesPerRow:W * 16];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLTexture> dst = (__bridge id<MTLTexture>)r.apply((__bridge void *)cb,
                                                          (__bridge void *)src, W, 1);
    if (!dst) return false;
    // Output is private RGBA16F: blit to a shared RGBA16F copy.
    MTLTextureDescriptor *rd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:1 mipmapped:NO];
    rd.storageMode = MTLStorageModeShared;
    id<MTLTexture> rb = [dev newTextureWithDescriptor:rd];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:dst toTexture:rb];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    std::vector<__fp16> h(W * 4);
    [rb getBytes:h.data() bytesPerRow:W * 8 fromRegion:MTLRegionMake2D(0, 0, W, 1) mipmapLevel:0];
    out.resize(W);
    for (int i = 0; i < W; ++i) {
        out[i] = {float(h[i * 4]), float(h[i * 4 + 1]), float(h[i * 4 + 2])};
    }
    return true;
}

// Scope: a synthetic 64×48 image of known colours through the real
// MetalScopeRenderer vs scope_math::bin on the CPU (with the OCIO CPU
// processor for the converted tier). Tap = source size (< 960), so each
// thread samples one texel centre exactly: counts must match bin for bin.
int scopeCheck(id<MTLDevice> dev, id<MTLCommandQueue> q, OCIOConfigManager &mgr,
               OCIO::ConstConfigRcPtr cfg, const char *label, const ScopeConfig &sc)
{
    const int W = 64, H = 48;
    std::vector<float> px(W * H * 4);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float *p = &px[(y * W + x) * 4];
            // A spread of saturated, neutral and out-of-range values.
            p[0] = (x % 8) / 7.0f * 1.1f - 0.05f;
            p[1] = (y % 6) / 5.0f;
            p[2] = ((x / 8 + y / 6) % 5) / 4.0f;
            p[3] = (x == 0 && y == 0) ? 0.0f : 1.0f;   // one transparent pixel, skipped
        }
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:H mipmapped:NO];
    id<MTLTexture> src = [dev newTextureWithDescriptor:td];
    [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px.data()
           bytesPerRow:W * 16];

    MetalScopeRenderer scope;
    scope.initialize();
    scope.setConfig(sc);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    if (!scope.encode((__bridge void *)cb, &mgr, (__bridge void *)src, W, H)) {
        std::printf("FAIL scope %s: encode skipped\n", label);
        return 1;
    }
    [cb commit];
    [cb waitUntilCompleted];
    std::vector<uint32_t> gpu, gpuOog;
    scope.readCounts(gpu, gpuOog);

    // CPU reference.
    const bool converted = sc.tier != ScopeTier::Signal;
    InterchangeSide side = InterchangeSide::None;
    OCIO::ConstCPUProcessorRcPtr cpu;
    if (converted) {
        auto t = OcioChainBuilder::buildScopeTransform(cfg, sc.colorspace, &side);
        cpu = cfg->getProcessor(t)->getDefaultCPUProcessor();
    }
    const ScopeAccumGpu u = scope_math::resolveAccum(sc, side, converted, W, H, 0);
    std::vector<uint32_t> ref(kScopeGrid * kScopeGrid, 0), refOog(kScopeGrid * kScopeGrid, 0);
    float refLevel = 0.0f, refChannel = 0.0f;
    for (int i = 0; i < W * H; ++i) {
        if (px[i * 4 + 3] <= 0.0f) continue;
        float rgb[3] = {px[i * 4], px[i * 4 + 1], px[i * 4 + 2]};
        if (cpu) cpu->applyRGB(rgb);
        const scope_math::ScopePixel sp = scope_math::classify(u, rgb);
        refLevel   = std::max(refLevel, scope_math::waveformLevel(u, sp));
        refChannel = std::max(refChannel, sp.channel);
        int bx, by; bool oog;
        scope_math::bin(u, rgb, bx, by, oog, ((i % W) + 0.5f) / W);
        ref[by * kScopeGrid + bx]++;
        if (oog) refOog[by * kScopeGrid + bx]++;
    }
    // OCIO's GPU path approximates some curves (PQ via a LUT), so a few
    // pixels can land one bin over. Count moved pixels, and fail if any
    // GPU count has no CPU count within ±2 bins.
    uint64_t total = 0, moved2 = 0, oogMoved2 = 0;
    int far = 0;
    for (int by = 0; by < kScopeGrid; ++by) {
        for (int bx = 0; bx < kScopeGrid; ++bx) {
            const size_t c = static_cast<size_t>(by) * kScopeGrid + bx;
            total += gpu[c];
            moved2 += gpu[c] > ref[c] ? gpu[c] - ref[c] : ref[c] - gpu[c];
            oogMoved2 += gpuOog[c] > refOog[c] ? gpuOog[c] - refOog[c] : refOog[c] - gpuOog[c];
            if (gpu[c] == 0) continue;
            bool near = false;
            for (int dy = -2; dy <= 2 && !near; ++dy) {
                for (int dx = -2; dx <= 2 && !near; ++dx) {
                    const int x = bx + dx, y = by + dy;
                    if (x >= 0 && y >= 0 && x < kScopeGrid && y < kScopeGrid)
                        near = ref[static_cast<size_t>(y) * kScopeGrid + x] > 0;
                }
            }
            if (!near) ++far;
        }
    }
    const int n = W * H - 1;
    // One bin is 1/512 of the scope — invisible. The image repeats ~240
    // colours, so one colour a bin over moves all its copies at once.
    const bool ok = total == static_cast<uint64_t>(n) && far == 0;
    std::printf("%s scope %-26s total %llu/%d, moved %llu px (≤ 2 bins), far %d, oog moved %llu\n",
                ok ? "ok  " : "FAIL", label, (unsigned long long)total, n,
                (unsigned long long)(moved2 / 2), far, (unsigned long long)(oogMoved2 / 2));
    QImage img;
    quint64 serial = 0;
    ScopePeaks peaks;
    if (!scope.latestImage(&img, &serial, &peaks) || img.isNull()) {
        std::printf("FAIL scope %s: no image\n", label);
        return 1;
    }
    bool peaksOk = true;
    if (sc.kind == ScopeKind::Waveform) {
        // OCIO's GPU PQ LUT vs the CPU curve: within 1 % (+ a hair).
        auto close = [](float g, float c) { return std::abs(g - c) <= 0.01f * std::abs(c) + 1e-3f; };
        peaksOk = peaks.sides == 1 && close(peaks.level[0], refLevel)
                  && close(peaks.channel[0], refChannel);
        std::printf("%s peak  %-26s level %.4f (cpu %.4f), channel %.4f (cpu %.4f)\n",
                    peaksOk ? "ok  " : "FAIL", label, peaks.level[0], refLevel,
                    peaks.channel[0], refChannel);
    }
    if (const char *dir = std::getenv("PROBE_SCOPE_DIR")) {
        QImage onBlack(img.size(), QImage::Format_RGB32);
        onBlack.fill(Qt::black);
        QPainter(&onBlack).drawImage(0, 0, img);
        onBlack.save(QString::fromUtf8(dir) + QLatin1Char('/')
                     + QString::fromUtf8(label).replace(QLatin1Char(' '), QLatin1Char('_'))
                     + QStringLiteral(".png"));
    }
    return ok && peaksOk ? 0 : 1;
}

// Peak pass at in-app size: one hot pixel in a 1920×1080 frame. The tap
// (960 wide, bilinear) would average it to ~1.6; the peak pass must see
// every pixel.
int hotPixelCheck(id<MTLDevice> dev, id<MTLCommandQueue> q, OCIOConfigManager &mgr)
{
    const int W = 1920, H = 1080;
    std::vector<__fp16> px(W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0.5f;
        px[i * 4 + 3] = 1.0f;
    }
    const int hot = (537 * W + 1333) * 4;   // odd row / column: between tap samples
    px[hot] = 5.0f; px[hot + 1] = 4.0f; px[hot + 2] = 3.0f;
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO];
    id<MTLTexture> src = [dev newTextureWithDescriptor:td];
    [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px.data() bytesPerRow:W * 8];
    ScopeConfig sc;
    sc.active = true;
    sc.kind = ScopeKind::Waveform;
    sc.tier = ScopeTier::Signal;
    MetalScopeRenderer scope;
    scope.initialize();
    scope.setConfig(sc);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    scope.encode((__bridge void *)cb, &mgr, (__bridge void *)src, W, H);
    [cb commit];
    [cb waitUntilCompleted];
    QImage img;
    ScopePeaks peaks;
    scope.latestImage(&img, nullptr, &peaks);
    // Y′ (709) of (5, 4, 3) = 0.2126·5 + 0.7152·4 + 0.0722·3.
    const float wantLevel = 0.2126f * 5.0f + 0.7152f * 4.0f + 0.0722f * 3.0f;
    const bool ok = peaks.sides == 1 && std::abs(peaks.level[0] - wantLevel) < 0.01f
                    && std::abs(peaks.channel[0] - 5.0f) < 0.01f;
    std::printf("%s peak  hot pixel 1920x1080          level %.4f (want %.4f), channel %.4f (want 5)\n",
                ok ? "ok  " : "FAIL", peaks.level[0], wantLevel, peaks.channel[0]);

    // Dual with A in a timeline gap: B's peak stays on side B, A unmeasured.
    sc.dual = true;
    MetalScopeRenderer gap;
    gap.initialize();
    gap.setConfig(sc);
    id<MTLCommandBuffer> cb2 = [q commandBuffer];
    const bool encoded = gap.encode((__bridge void *)cb2, &mgr, nullptr, 0, 0,
                                    (__bridge void *)src, W, H);
    [cb2 commit];
    [cb2 waitUntilCompleted];
    ScopePeaks gp;
    gap.latestImage(&img, nullptr, &gp);
    const bool gapOk = encoded && gp.sides == 2 && !gp.measured[0] && gp.measured[1]
                       && std::abs(gp.level[1] - wantLevel) < 0.01f && gp.level[0] == 0.0f;
    std::printf("%s peak  dual, A in a gap                sides %d, measured A %d B %d, B level %.4f\n",
                gapOk ? "ok  " : "FAIL", gp.sides, gp.measured[0], gp.measured[1], gp.level[1]);
    return ok && gapOk ? 0 : 1;
}

// Config upgrade guard: every scope tag-rule list (color/scope_names.h)
// must name a usable colourspace — known, not data, interchange role on its
// side — in the scopes' fallback config (Blender 5.2: failures), and is
// reported for the other bundled configs (a miss there just means the
// scopes fall back to Blender 5.2 for that file kind).
int namesCheck(const std::string &root)
{
    int failures = 0;
    for (const char *dir : {"Blender5.2", "ACES_2.0", "Blender5.1", "ACES_1.3"}) {
        const bool required = std::string(dir) == "Blender5.2";
        OCIO::ConstConfigRcPtr cfg;
        try {
            cfg = OCIO::Config::CreateFromFile((root + "/" + dir + "/config.ocio").c_str());
        } catch (const OCIO::Exception &e) {
            std::printf("%s names %-11s config failed to load: %s\n",
                        required ? "FAIL" : "--  ", dir, e.what());
            failures += required;
            continue;
        }
        for (const auto &l : scope_names::kAll) {
            const char *found = nullptr;
            for (int k = 0; k < l.count && !found; ++k) {
                OCIO::ConstColorSpaceRcPtr cs = cfg->getColorSpace(l.names[k]);
                if (!cs || cs->isData()) continue;
                const bool scene = cs->getReferenceSpaceType() == OCIO::REFERENCE_SPACE_SCENE;
                if (cfg->hasRole(scene ? "aces_interchange" : "cie_xyz_d65_interchange")) {
                    found = l.names[k];
                }
            }
            const bool ok = found != nullptr;
            std::printf("%s names %-11s %-20s %s\n", ok ? "ok  " : (required ? "FAIL" : "--  "),
                        dir, l.label, ok ? found : "(none — scopes use the built-in config)");
            if (!ok && required) ++failures;
        }
    }
    return failures;
}

// In-app conditions: 1920×1080 RGBA16F bars, tapped at 960×540.
void barsCheck(id<MTLDevice> dev, id<MTLCommandQueue> q, OCIOConfigManager &mgr,
               const ScopeConfig &sc)
{
    const int W = 1920, H = 1080;
    std::vector<__fp16> px(W * H * 4);
    const float bars[6][3] = {{1,0,0},{0,1,0},{0,0,1},{0,1,1},{1,0,1},{1,1,0}};
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        __fp16 *p = &px[(y * W + x) * 4];
        const float *c = bars[std::min(x / 320, 5)];
        p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = 1;
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO];
    id<MTLTexture> src = [dev newTextureWithDescriptor:td];
    [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px.data() bytesPerRow:W * 8];
    MetalScopeRenderer scope;
    scope.initialize();
    scope.setConfig(sc);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    scope.encode((__bridge void *)cb, &mgr, (__bridge void *)src, W, H);
    [cb commit];
    [cb waitUntilCompleted];
    std::vector<uint32_t> g, o;
    scope.readCounts(g, o);
    std::printf("bars check (%s):", qPrintable(sc.colorspace));
    for (int c = 0; c < kScopeGrid * kScopeGrid; ++c)
        if (g[c] > 1000) std::printf(" (%d,%d)=%u", c % kScopeGrid, c / kScopeGrid, g[c]);
    std::printf("\n");
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: probe-ocio-metal <assets/OCIO dir>\n");
        return 1;
    }
    const std::string root = argv[1];
    QWindow win;
    win.setSurfaceType(QSurface::MetalSurface);
    win.resize(16, 16);
    win.create();
    if (!MetalDeviceManager::instance().initialize(&win)) return 1;
    id<MTLDevice> dev = (__bridge id<MTLDevice>)MetalDeviceManager::instance().device();
    id<MTLCommandQueue> q = [dev newCommandQueue];

    const Case cases[] = {
        {"Blender5.2", "Rec.2100-PQ", "sRGB", "Standard"},
        {"Blender5.2", "Linear Rec.709", "sRGB", "AgX"},
        {"Blender5.2", "Rec.1886", "Linear sRGB EDR", "Standard (No Tonemap)"},
        {"ACES_2.0", "ACEScg", "sRGB - Display", "ACES 2.0 - SDR 100 nits (Rec.709)"},
        {"ACES_2.0", "Rec.2100-PQ - Display", "Rec.2100-PQ - Display", "Un-tone-mapped"},
        {"ACES_2.0", "ARRI LogC4", "Rec.1886 Rec.709 - Display", "ACES 2.0 - SDR 100 nits (Rec.709)"},
    };
    struct Variant { const char *name; LinearStageSettings st; ViewerAids va; };
    LinearStageSettings gain2;  gain2.gain = 2.0f;
    LinearStageSettings knee;   knee.kneeEnabled = true; knee.kneeSourceNits = 1000.0f;
    ViewerAids gamma2;   gamma2.gamma = 2.0f;
    ViewerAids luma;     luma.channel = ChannelView::Luma;
    ViewerAids alpha;    alpha.channel = ChannelView::Alpha;
    ViewerAids green;    green.channel = ChannelView::Green;
    const Variant variants[] = {{"identity", LinearStageSettings{}, ViewerAids{}},
                                {"gain 2", gain2, ViewerAids{}},
                                {"knee 1000", knee, ViewerAids{}},
                                {"gamma 2", LinearStageSettings{}, gamma2},
                                {"luma", LinearStageSettings{}, luma},
                                {"alpha", LinearStageSettings{}, alpha},
                                {"knee+gam", knee, gamma2},
                                {"green", gain2, green}};

    int failures = namesCheck(root);
    for (const Case &c : cases) {
        const std::string cfgPath = root + "/" + c.config + "/config.ocio";
        OCIOConfigManager mgr;
        if (!mgr.loadConfigFile(QString::fromStdString(cfgPath))) return 1;
        mgr.setActiveInput(QString::fromUtf8(c.input));
        mgr.setActiveDisplay(QString::fromUtf8(c.display));
        mgr.setActiveView(QString::fromUtf8(c.view));
        auto cfg = OCIO::Config::CreateFromFile(cfgPath.c_str());
        for (const Variant &v : variants) {
            MetalOcioRenderer r;
            r.initialize();
            r.setStage(v.st);
            r.setViewer(v.va);
            if (!r.rebuild(&mgr)) {
                std::printf("FAIL build  %s | %s | %s / %s [%s]: %s\n", c.config, c.input,
                            c.display, c.view, v.name, qPrintable(r.lastError()));
                ++failures;
                continue;
            }
            std::vector<std::array<float, 3>> gpu, cpu;
            cpuRef(mgr, cfg, v.st, v.va, cpu);
            if (!gpuRun(dev, q, r, gpu)) { ++failures; continue; }
            float worst = 0.0f;
            for (size_t i = 0; i < gpu.size(); ++i) {
                for (int k = 0; k < 3; ++k) {
                    worst = std::max(worst, std::abs(gpu[i][k] - cpu[i][k])
                                                / std::max(1.0f, std::abs(cpu[i][k])));
                }
            }
            const bool ok = worst < 4e-3f;   // RGBA16F output
            if (!ok) ++failures;
            std::printf("%s %-9s split=%d  worst %.1e  %s | %s | %s / %s\n", ok ? "ok  " : "FAIL",
                        v.name, r.stageActive() ? 1 : 0, worst, c.config, c.input, c.display,
                        c.view);
        }
    }
    // ---- Scope ----
    {
        const std::string cfgPath = root + "/Blender5.2/config.ocio";
        OCIOConfigManager mgr;
        mgr.loadConfigFile(QString::fromStdString(cfgPath));
        auto cfg = OCIO::Config::CreateFromFile(cfgPath.c_str());
        ScopeConfig sc;
        sc.active = true;
        sc.tier = ScopeTier::Signal;
        failures += scopeCheck(dev, q, mgr, cfg, "signal 709", sc);
        sc.zoom = 2;
        failures += scopeCheck(dev, q, mgr, cfg, "signal 709 zoom 2", sc);
        sc.zoom = 1;
        sc.tier = ScopeTier::Input;
        sc.colorspace = "Rec.1886";
        sc.scale = ScopeScale::Sdr;
        failures += scopeCheck(dev, q, mgr, cfg, "input Rec.1886 SDR", sc);
        sc.colorspace = "Rec.2100-PQ";
        sc.scale = ScopeScale::Hdr;
        failures += scopeCheck(dev, q, mgr, cfg, "input Rec.2100-PQ HDR", sc);
        sc.colorspace = "Linear Rec.2020";
        sc.scale = ScopeScale::Sdr;
        failures += scopeCheck(dev, q, mgr, cfg, "input Linear 2020 SDR", sc);
        ScopeConfig wf;
        wf.active = true;
        wf.kind = ScopeKind::Waveform;
        wf.tier = ScopeTier::Signal;
        failures += scopeCheck(dev, q, mgr, cfg, "waveform signal", wf);
        wf.tier = ScopeTier::Input;
        wf.colorspace = "Rec.2100-PQ";
        wf.scale = ScopeScale::Hdr;
        failures += scopeCheck(dev, q, mgr, cfg, "waveform nits HDR 1k", wf);
        wf.waveformPeakNits = 4000;
        failures += scopeCheck(dev, q, mgr, cfg, "waveform nits HDR 4k", wf);
        wf.waveformPeakNits = 300;
        failures += scopeCheck(dev, q, mgr, cfg, "waveform nits HDR 300", wf);
        failures += hotPixelCheck(dev, q, mgr);
    }
    {
        const std::string cfgPath = root + "/ACES_2.0/config.ocio";
        OCIOConfigManager mgr;
        mgr.loadConfigFile(QString::fromStdString(cfgPath));
        auto cfg = OCIO::Config::CreateFromFile(cfgPath.c_str());
        ScopeConfig sc;
        sc.active = true;
        sc.tier = ScopeTier::Assumed;
        sc.colorspace = "Rec.1886 Rec.709 - Display";
        sc.scale = ScopeScale::Sdr;
        failures += scopeCheck(dev, q, mgr, cfg, "ACES Rec.1886 SDR", sc);
        barsCheck(dev, q, mgr, sc);
        sc.tier = ScopeTier::Signal;
        barsCheck(dev, q, mgr, sc);
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 2 : 0;
}
