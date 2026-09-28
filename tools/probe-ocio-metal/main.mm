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

#include <QGuiApplication>
#include <QWindow>

#import <Metal/Metal.h>

#include <OpenColorIO/OpenColorIO.h>

#include <array>
#include <cmath>
#include <cstdio>
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
            const LinearStageSettings &st, std::vector<std::array<float, 3>> &out)
{
    out = kValues;
    if (linear_stage::isIdentity(st)) {
        auto cpu = cfg->getProcessor(OcioChainBuilder::buildGroupTransform(&mgr, cfg))
                       ->getDefaultCPUProcessor();
        for (auto &v : out) cpu->applyRGB(v.data());
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
    struct Variant { const char *name; LinearStageSettings st; };
    LinearStageSettings gain2;  gain2.gain = 2.0f;
    LinearStageSettings knee;   knee.kneeEnabled = true; knee.kneeSourceNits = 1000.0f;
    const Variant variants[] = {{"identity", LinearStageSettings{}}, {"gain 2", gain2},
                                {"knee 1000", knee}};

    int failures = 0;
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
            if (!r.rebuild(&mgr)) {
                std::printf("FAIL build  %s | %s | %s / %s [%s]: %s\n", c.config, c.input,
                            c.display, c.view, v.name, qPrintable(r.lastError()));
                ++failures;
                continue;
            }
            std::vector<std::array<float, 3>> gpu, cpu;
            cpuRef(mgr, cfg, v.st, cpu);
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
    std::printf("%d failure(s)\n", failures);
    return failures ? 2 : 0;
}
