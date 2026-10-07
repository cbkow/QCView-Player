// MetalOcioRenderer — see header for adaptation notes.

#include "metal_ocio_renderer.h"

#include "color/ocio_chain_builder.h"
#include "metal_device_manager.h"
#include "metal_ocio_luts.h"

#import <Metal/Metal.h>

#include <QFile>
#include <QtLogging>

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

namespace {

// Compute-kernel template that wraps the OCIO function(s). Bindings:
//   texture(0)  = source (RGBA16F, decoded frame)
//   texture(1)  = output (RGBA16F, post-OCIO)
//   texture(2..) = OCIO LUTs, in declaration order
//   sampler(0)  = src_smp (linear, clamp) — also passed for every LUT:
//                 OCIO's LUT samplers are all linear + clamp, and one
//                 shared sampler keeps a split chain (two OCIO functions)
//                 inside Metal's 16-sampler limit.
//   buffer(0)   = QcvStage (split chain only)
//   buffer(1)   = QcvViewer (viewer aids, every kernel)
// OCIO's emitted MSL function takes its textures + samplers as
// arguments, so the wrapper is composed at rebuild() time once the LUT
// inventory is known.
constexpr const char *kKernelHeader = R"(
#include <metal_stdlib>
using namespace metal;

)";

constexpr const char *kKernelMainPrefix = R"(
kernel void ocio_apply(
    texture2d<float, access::sample>  src     [[texture(0)]],
    texture2d<float, access::write>   dst     [[texture(1)]],
    sampler                           src_smp [[sampler(0)]],
)";

constexpr const char *kKernelMainSuffix = R"(
    uint2 gid                                  [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float2 uv = (float2(gid) + 0.5) / float2(dst.get_width(), dst.get_height());
    float4 color = src.sample(src_smp, uv);
)";

constexpr const char *kKernelFooter = R"(
    dst.write(color, gid);
}
)";

// Everything a rebuild produces; swapped in whole.
struct Built {
    id<MTLComputePipelineState> pipeline = nil;
    std::vector<id<MTLTexture>> luts;          // in kernel declaration order
    bool            minColor = false;          // the fixed minColor kernel (blocks are uniforms)
    bool            split = false;
    InterchangeSide side = InterchangeSide::None;
    bool            displayIsSdr = true;
    OutputEncoding  encoding = OutputEncoding::Sdr;
    int             outputPrimaries = 0;
    QString         error;
};

void dumpFailure(const QString &kernel, const QString &ocioText)
{
    const QString fullPath = QStringLiteral("/tmp/qcv-ocio-kernel-fail.metal");
    const QString ocioOnly = QStringLiteral("/tmp/qcv-ocio-chain-only.metal");
    if (QFile f(fullPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(kernel.toUtf8());
        qInfo("MetalOcioRenderer: dumped failing kernel to %s", qPrintable(fullPath));
    }
    if (QFile f(ocioOnly); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(ocioText.toUtf8());
        qInfo("MetalOcioRenderer: dumped OCIO chain text to %s", qPrintable(ocioOnly));
    }
}

// Compile `kernel` into a compute pipeline. Returns nil (and sets error)
// on failure.
id<MTLComputePipelineState> compileKernel(id<MTLDevice> device, const QString &kernel,
                                          const QString &ocioText, QString &error,
                                          const char *entry = "ocio_apply",
                                          bool preciseMath = false)
{
    NSError *err = nil;
    NSString *src = [NSString stringWithUTF8String:kernel.toUtf8().constData()];
    // The minColor core is verified against the upstream DCTL float for
    // float; fast math (Metal's default) reassociates its way to NaN in
    // the hue / purity functions on saturated input (a white rendered
    // green). Same setting as minColorAE's Metal host.
    MTLCompileOptions *opts = nil;
    if (preciseMath) {
        opts = [[MTLCompileOptions alloc] init];
        opts.fastMathEnabled = NO;
    }
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:opts error:&err];
    if (!lib) {
        error = QStringLiteral("MetalOcioRenderer: kernel compile failed: %1")
            .arg(err ? QString::fromUtf8([err.localizedDescription UTF8String])
                     : QStringLiteral("(no error)"));
        qWarning("%s", qPrintable(error));
        dumpFailure(kernel, ocioText);
        return nil;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:entry]];
    if (!fn) {
        error = QStringLiteral("MetalOcioRenderer: %1 not found").arg(QString::fromLatin1(entry));
        return nil;
    }
    id<MTLComputePipelineState> pso =
        [device newComputePipelineStateWithFunction:fn error:&err];
    if (!pso) {
        error = QStringLiteral("MetalOcioRenderer: pipeline creation failed: %1")
            .arg(err ? QString::fromUtf8([err.localizedDescription UTF8String])
                     : QStringLiteral("(no error)"));
        qWarning("%s", qPrintable(error));
    }
    return pso;
}

// Build a pipeline for `spec`. Pure function of its inputs — safe on a
// background queue (MTLDevice is thread-safe; the spec is a value).
Built buildPipeline(id<MTLDevice> device, const OcioChainSpec &spec,
                    bool sdrCapture, bool wantSplit)
{
    Built out;
    // The spec carries its SDR capture Display/View.
    DisplayViewOverride sdr;
    const DisplayViewOverride *ov =
        sdrCapture ? DisplayViewOverride::sdrFor(spec, sdr) : nullptr;

    if (wantSplit) {
        OcioSplitChain chain =
            OcioChainBuilder::buildSplit(spec, OcioChainBuilder::Language::Msl_2_0, ov);
        if (chain.ok) {
            int nextTex = 2;   // 0 = src, 1 = dst
            QString args, preCall, postCall;
            if (!gatherLuts(device, chain.pre.desc, nextTex, out.luts, args, preCall, out.error) ||
                !gatherLuts(device, chain.post.desc, nextTex, out.luts, args, postCall, out.error)) {
                return out;
            }
            const QString kernel =
                QString::fromUtf8(kKernelHeader)
                + QString::fromUtf8(kLinearStageMsl)
                + chain.pre.shaderText
                + chain.post.shaderText
                + QString::fromUtf8(kKernelMainPrefix)
                + args
                + QStringLiteral("    constant QcvStage &qcvStage [[buffer(0)]],\n")
                + QStringLiteral("    constant QcvViewer &qcvView [[buffer(1)]],\n")
                + QString::fromUtf8(kKernelMainSuffix)
                + QStringLiteral("    color = OCIOPre(%1color);\n").arg(preCall)
                + QStringLiteral("    color = qcvLinearStage(color, qcvStage);\n")
                + QStringLiteral("    color = OCIOPost(%1color);\n").arg(postCall)
                + QStringLiteral("    color = qcvViewerApply(color, qcvView);\n")
                + QString::fromUtf8(kKernelFooter);
            out.pipeline = compileKernel(device, kernel,
                                         chain.pre.shaderText + chain.post.shaderText, out.error);
            if (!out.pipeline) out.luts.clear();
            out.split           = true;
            out.side            = chain.side;
            out.displayIsSdr    = chain.displayIsSdr;
            out.encoding        = chain.encoding;
            out.outputPrimaries = chain.outputPrimaries;
            return out;
        }
        // No interchange role / data colourspace or view: the stage is
        // unavailable for this chain — run it unsplit.
        qInfo("MetalOcioRenderer: stage unavailable (%s) — unsplit chain",
              qPrintable(chain.errorMessage));
    }

    OcioChain chain = OcioChainBuilder::build(spec, OcioChainBuilder::Language::Msl_2_0, ov);
    if (!chain.ok) {
        out.error = chain.errorMessage;
        return out;
    }
    int nextTex = 2;
    QString args, call;
    if (!gatherLuts(device, chain.desc, nextTex, out.luts, args, call, out.error)) return out;
    // OCIODisplay() takes textures + samplers FIRST, inPixel LAST. With
    // zero LUTs `call` is empty and the call collapses to
    // OCIODisplay(color), the no-LUT signature OCIO emits.
    const QString kernel =
        QString::fromUtf8(kKernelHeader)
        + QString::fromUtf8(kLinearStageMsl)     // viewer aids + PQ helpers
        + chain.shaderText
        + QString::fromUtf8(kKernelMainPrefix)
        + args
        + QStringLiteral("    constant QcvViewer &qcvView [[buffer(1)]],\n")
        + QString::fromUtf8(kKernelMainSuffix)
        + QStringLiteral("    color = OCIODisplay(%1color);\n").arg(call)
        + QStringLiteral("    color = qcvViewerApply(color, qcvView);\n")
        + QString::fromUtf8(kKernelFooter);
    out.encoding        = chain.encoding;
    out.outputPrimaries = chain.outputPrimaries;
    out.pipeline = compileKernel(device, kernel, chain.shaderText, out.error);
    if (!out.pipeline) out.luts.clear();
    return out;
}

} // namespace

// ---- minColor: the OCIO-free engine's kernel ---------------------------
// The vendored minColorAE core (color/mincolor/, see VENDORED.md) as text
// from the resource bundle, concatenated the way opendrt_shim_msl.h
// documents, with QCView's viewer aids after it. One kernel, compiled
// once; the chain is four uniform blocks (MinColorGpu):
//   buffer(0) DrtParams in      Input half: file encoding → linear Rec.2020, knee fields
//   buffer(1) DrtParams out     Output half: rendering + display encoding
//   buffer(2) DrtAgxParams      AgX
//   buffer(3) QcvMinColorFlags  knee on, AgX on, output scale (EDR)
//   buffer(4) QcvViewer         viewer aids (gamma, channel view)
constexpr const char *kMinColorMain = R"(
struct QcvMinColorFlags { int kneeOn; int agxOn; float outScale; int pad; };

kernel void mincolor_apply(
    texture2d<float, access::sample>  src     [[texture(0)]],
    texture2d<float, access::write>   dst     [[texture(1)]],
    sampler                           src_smp [[sampler(0)]],
    constant DrtParams               &pIn     [[buffer(0)]],
    constant DrtParams               &pOut    [[buffer(1)]],
    constant DrtAgxParams            &agx     [[buffer(2)]],
    constant QcvMinColorFlags        &fl      [[buffer(3)]],
    constant QcvViewer               &qcvView [[buffer(4)]],
    uint2 gid                                  [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float2 uv = (float2(gid) + 0.5) / float2(dst.get_width(), dst.get_height());
    float4 color = src.sample(src_smp, uv);
    float3 c = color.rgb;
    c = drt_input_transform(pIn, c);
    if (fl.kneeOn != 0) c = drt_knee(pIn, c);
    if (fl.agxOn != 0)  c = drt_agx(agx, c);
    c = drt_transform(pOut, c);
    color.rgb = c * fl.outScale;
    color = qcvViewerApply(color, qcvView);
    dst.write(color, gid);
}
)";

QString resourceText(const char *path, QString &error)
{
    QFile f(QString::fromLatin1(path));
    if (!f.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("MetalOcioRenderer: missing kernel resource %1").arg(QString::fromLatin1(path));
        return {};
    }
    return QString::fromUtf8(f.readAll());
}

Built buildMinColorPipeline(id<MTLDevice> device)
{
    Built out;
    out.minColor = true;
    QString kernel = QString::fromUtf8(kKernelHeader) + QString::fromUtf8(kLinearStageMsl);
    for (const char *part : {":/mincolor/opendrt_shim_msl.h", ":/mincolor/opendrt_params.h",
                             ":/mincolor/opendrt_kernel.h", ":/mincolor/mincolor_knee.h",
                             ":/mincolor/mincolor_agx.h"}) {
        const QString text = resourceText(part, out.error);
        if (!out.error.isEmpty()) return out;
        kernel += QStringLiteral("\n// ---- ") + QString::fromLatin1(part) + QStringLiteral("\n") + text;
    }
    kernel += QString::fromUtf8(kMinColorMain);
    out.pipeline = compileKernel(device, kernel, QString(), out.error, "mincolor_apply",
                                 /*preciseMath=*/true);
    if (!out.pipeline) {
        const QString path = QStringLiteral("/tmp/qcv-mincolor-kernel-fail.metal");
        if (QFile f(path); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(kernel.toUtf8());
            qInfo("MetalOcioRenderer: dumped failing minColor kernel to %s", qPrintable(path));
        }
    }
    return out;
}

struct MetalOcioRenderer::Impl {
    id<MTLDevice>       device  = nil;
    id<MTLSamplerState> sampler = nil;

    Built active;
    OcioChainSpec activeSpec;          // the chain the active build (or its failure) is for
    bool  activeValid = false;         // … false = nothing built yet / invalidated
    bool  activeSplit = false;         // ... and whether it was asked to split

    // Background compile (async instances). One job at a time; the
    // result waits in `pending` until the render thread swaps it in.
    dispatch_queue_t queue = nullptr;
    std::mutex       pendingMutex;
    std::unique_ptr<Built> pending;
    OcioChainSpec pendingSpec;
    bool  pendingSplit = false;
    bool  jobRunning   = false;
    OcioChainSpec jobSpec;
    bool  jobSplit     = false;

    // Built pipelines kept for reuse (most recent first) — prewarm() fills
    // it, so a chain change (a playlist cut, a clip switch) swaps
    // instantly. Failed keys are remembered so a broken chain isn't
    // rebuilt every frame.
    struct CacheEntry { OcioChainSpec spec; bool split = false; Built built; };
    std::deque<CacheEntry> cache;
    static constexpr size_t kCacheSize = 8;
    std::vector<std::pair<OcioChainSpec, bool>> failed;

    static bool sameKey(const OcioChainSpec &a, bool sa, const OcioChainSpec &b, bool sb)
    {
        return sa == sb && a.sameShader(b);
    }
    void remember(const OcioChainSpec &spec, bool split, const Built &b)
    {
        if (!b.pipeline) {
            if (failed.size() >= 16) failed.erase(failed.begin());
            failed.emplace_back(spec, split);
            return;
        }
        cache.erase(std::remove_if(cache.begin(), cache.end(),
                                   [&](const CacheEntry &e) { return sameKey(e.spec, e.split, spec, split); }),
                    cache.end());
        cache.push_front({spec, split, b});
        while (cache.size() > kCacheSize) cache.pop_back();
    }
    const Built *cached(const OcioChainSpec &spec, bool split) const
    {
        for (const CacheEntry &e : cache)
            if (sameKey(e.spec, e.split, spec, split)) return &e.built;
        return nullptr;
    }
    bool knownFailed(const OcioChainSpec &spec, bool split) const
    {
        for (const auto &f : failed)
            if (sameKey(f.first, f.second, spec, split)) return true;
        return false;
    }

    LinearStageSettings stage;
    ViewerAids          viewer;
    bool  async      = false;
    bool  sdrCapture = false;

    // minColor engine: one kernel for the instance's life, compiled on
    // the compile queue at initialize() (async instances) or on first use
    // (capture instances); the chain arrives as uniform blocks with
    // every rebuild(). A failure is final for the session.
    Built        mcBuilt;
    bool         mcTried = false;
    std::mutex   mcMutex;
    std::unique_ptr<Built> mcPending;
    bool         mcJobRunning = false;
    MinColorGpu  mcBlocks;

    id<MTLTexture> outputTex = nil;
    int            outputW   = 0;
    int            outputH   = 0;

    QString lastError;

    void install(Built &&b, const OcioChainSpec &spec, bool split)
    {
        if (b.pipeline || !active.pipeline) {
            // A failed build doesn't replace a working pipeline; it is
            // recorded against its key so it isn't retried every frame
            // (the next chain change gets a fresh attempt).
            active = std::move(b);
        } else {
            lastError = b.error;
        }
        if (active.pipeline) lastError.clear();
        else                 lastError = active.error;
        activeSpec  = spec;
        activeValid = true;
        activeSplit = split;
    }
};

MetalOcioRenderer::MetalOcioRenderer()
    : m_impl(new Impl())
{
}

MetalOcioRenderer::~MetalOcioRenderer()
{
    shutdown();
    delete m_impl;
}

bool MetalOcioRenderer::initialize()
{
    auto &mgr = MetalDeviceManager::instance();
    m_impl->device = (__bridge id<MTLDevice>)mgr.device();
    if (!m_impl->device) {
        qWarning("MetalOcioRenderer: no Metal device");
        return false;
    }

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter    = MTLSamplerMinMagFilterLinear;
    sd.magFilter    = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    m_impl->sampler = [m_impl->device newSamplerStateWithDescriptor:sd];
    if (!m_impl->queue) {
        m_impl->queue = dispatch_queue_create("qcv.ocio.compile", DISPATCH_QUEUE_SERIAL);
    }
    if (m_impl->sampler && m_impl->async) {
        // The minColor kernel, ahead of its first use so an engine switch
        // never waits on a compile (plan: "switching and safety").
        Impl *impl = m_impl;
        id<MTLDevice> device = m_impl->device;
        std::lock_guard lock(impl->mcMutex);
        if (!impl->mcJobRunning && !impl->mcTried) {
            impl->mcJobRunning = true;
            dispatch_async(impl->queue, ^{
                auto b = std::make_unique<Built>(buildMinColorPipeline(device));
                std::lock_guard lk(impl->mcMutex);
                impl->mcPending    = std::move(b);
                impl->mcJobRunning = false;
            });
        }
    }
    return m_impl->sampler != nil;
}

void MetalOcioRenderer::shutdown()
{
    if (!m_impl) return;
    // Drain a running compile before dropping the device it uses.
    if (m_impl->queue) dispatch_sync(m_impl->queue, ^{});
    {
        std::lock_guard lock(m_impl->pendingMutex);
        m_impl->pending.reset();
        m_impl->jobRunning = false;
    }
    m_impl->active      = Built{};
    m_impl->activeValid = false;
    m_impl->cache.clear();
    m_impl->failed.clear();
    m_impl->sampler   = nil;
    m_impl->outputTex = nil;
    m_impl->outputW   = 0;
    m_impl->outputH   = 0;
    m_impl->device    = nil;
}

bool MetalOcioRenderer::isInitialized() const
{
    return m_impl && m_impl->device != nil && m_impl->sampler != nil;
}

bool MetalOcioRenderer::hasPipeline() const
{
    return m_impl && m_impl->active.pipeline != nil;
}

bool MetalOcioRenderer::stageActive() const
{
    return m_impl && m_impl->active.pipeline != nil && m_impl->active.split;
}

const QString &MetalOcioRenderer::lastError() const
{
    return m_impl->lastError;
}

void MetalOcioRenderer::setSdrCapture(bool on)
{
    m_impl->sdrCapture  = on;
    m_impl->activeValid = false;
}

void MetalOcioRenderer::setStage(const LinearStageSettings &stage)
{
    m_impl->stage = stage;
}

void MetalOcioRenderer::setViewer(const ViewerAids &viewer)
{
    m_impl->viewer = viewer;
}

void MetalOcioRenderer::setAsync(bool on)
{
    m_impl->async = on;
}

bool MetalOcioRenderer::rebuild(const OcioChainSpec &spec)
{
    if (!spec.complete() || !isInitialized()) return false;
    Impl &i = *m_impl;

    if (spec.engine == ColorEngine::MinColor) {
        // The chain is uniforms: take this frame's blocks, make sure the
        // one kernel exists, and make it active. A background compile
        // (initialize) lands here; a capture instance compiles in line.
        i.mcBlocks = i.sdrCapture ? spec.minColorSdr : spec.minColor;
        {
            std::lock_guard lock(i.mcMutex);
            if (i.mcPending) {
                i.mcBuilt = std::move(*i.mcPending);
                i.mcPending.reset();
                i.mcTried = true;
                if (i.mcBuilt.pipeline) qInfo("MetalOcioRenderer: minColor kernel ready (built ahead)");
                else qWarning("MetalOcioRenderer: minColor kernel failed: %s", qPrintable(i.mcBuilt.error));
            }
        }
        if (!i.mcTried) {
            bool running = false;
            { std::lock_guard lock(i.mcMutex); running = i.mcJobRunning; }
            if (running) return i.active.pipeline != nil;   // keep drawing what we have
            i.mcBuilt = buildMinColorPipeline(i.device);
            i.mcTried = true;
            if (i.mcBuilt.pipeline) qInfo("MetalOcioRenderer: minColor kernel compiled");
            else qWarning("MetalOcioRenderer: minColor kernel failed: %s", qPrintable(i.mcBuilt.error));
        }
        if (!i.mcBuilt.pipeline) {
            i.lastError = i.mcBuilt.error;
            return i.active.pipeline != nil && i.active.minColor;
        }
        if (!(i.activeValid && i.active.minColor)) {
            i.install(Built(i.mcBuilt), spec, /*split=*/false);
        }
        return true;
    }

    const bool wantSplit = !linear_stage::isIdentity(i.stage);

    // 1. A finished background build (the live chain, or a prewarm):
    //    into the cache; installed when it is the chain asked for.
    if (i.async) {
        std::unique_ptr<Built> ready;
        OcioChainSpec readySpec;
        bool readySplit = false;
        {
            std::lock_guard lock(i.pendingMutex);
            if (i.pending) {
                ready = std::move(i.pending);
                readySpec = i.pendingSpec;
                readySplit = i.pendingSplit;
            }
        }
        if (ready) {
            i.remember(readySpec, readySplit, *ready);
            if (Impl::sameKey(readySpec, readySplit, spec, wantSplit)) {
                i.install(std::move(*ready), readySpec, readySplit);
                if (i.active.pipeline) {
                    qInfo("MetalOcioRenderer: swapped in chain '%s' (%s, %zu LUTs)",
                          qPrintable(readySpec.scene.input), i.active.split ? "split" : "unsplit",
                          i.active.luts.size());
                }
            }
        }
    }

    // 2. Already current?
    if (i.activeValid && i.activeSpec.sameShader(spec) && wantSplit == i.activeSplit) {
        return i.active.pipeline != nil;
    }

    // 2b. Built ahead of time (prewarm) or earlier: swap it in now.
    if (const Built *hit = i.cached(spec, wantSplit)) {
        i.install(Built(*hit), spec, wantSplit);
        qInfo("MetalOcioRenderer: chain '%s' ready (built ahead)", qPrintable(spec.scene.input));
        return i.active.pipeline != nil;
    }

    // 3. Synchronous build: capture instances, and the first build of a
    //    session (nothing to keep showing meanwhile).
    if (!i.async || !i.active.pipeline) {
        Built b = buildPipeline(i.device, spec, i.sdrCapture, wantSplit);
        i.remember(spec, wantSplit, b);
        i.install(std::move(b), spec, wantSplit);
        if (i.active.pipeline) {
            qInfo("MetalOcioRenderer: rebuilt for chain '%s' (%s, %zu LUTs)",
                  qPrintable(spec.scene.input), i.active.split ? "split" : "unsplit",
                  i.active.luts.size());
        }
        return i.active.pipeline != nil;
    }

    // 4. Background build; keep the current pipeline until it lands.
    {
        std::lock_guard lock(i.pendingMutex);
        if (i.jobRunning) {
            return true;   // one job at a time; re-evaluated when it lands
        }
        i.jobRunning = true;
        i.jobSpec    = spec;
        i.jobSplit   = wantSplit;
    }
    id<MTLDevice> device = i.device;
    const bool sdrCapture = i.sdrCapture;
    Impl *impl = m_impl;
    const OcioChainSpec jobSpec = spec;
    dispatch_async(i.queue, ^{
        auto b = std::make_unique<Built>(buildPipeline(device, jobSpec, sdrCapture, wantSplit));
        std::lock_guard lock(impl->pendingMutex);
        impl->pending      = std::move(b);
        impl->pendingSpec  = jobSpec;
        impl->pendingSplit = wantSplit;
        impl->jobRunning   = false;
    });
    return true;
}

void MetalOcioRenderer::prewarm(const std::vector<OcioChainSpec> &specs, float gain)
{
    if (!isInitialized() || !m_impl->async) return;
    Impl &i = *m_impl;
    for (const OcioChainSpec &spec : specs) {
        if (!spec.complete()) continue;
        const bool split = !linear_stage::isIdentity(spec.stage(gain));
        if (i.activeValid && Impl::sameKey(i.activeSpec, i.activeSplit, spec, split)) continue;
        if (i.cached(spec, split) || i.knownFailed(spec, split)) continue;
        {
            std::lock_guard lock(i.pendingMutex);
            // One job at a time, and never over a finished build that
            // rebuild() hasn't collected yet.
            if (i.jobRunning || i.pending) return;
            i.jobRunning = true;
            i.jobSpec    = spec;
            i.jobSplit   = split;
        }
        id<MTLDevice> device = i.device;
        const bool sdrCapture = i.sdrCapture;
        Impl *impl = m_impl;
        const OcioChainSpec jobSpec = spec;
        dispatch_async(i.queue, ^{
            auto b = std::make_unique<Built>(buildPipeline(device, jobSpec, sdrCapture, split));
            std::lock_guard lock(impl->pendingMutex);
            impl->pending      = std::move(b);
            impl->pendingSpec  = jobSpec;
            impl->pendingSplit = split;
            impl->jobRunning   = false;
        });
        qInfo("MetalOcioRenderer: building chain '%s' ahead of time",
              qPrintable(spec.scene.input));
        return;
    }
}

void *MetalOcioRenderer::apply(void *cmdBufPtr, void *sourceMtlTexture,
                               int width, int height)
{
    if (!hasPipeline() || !cmdBufPtr || !sourceMtlTexture ||
        width <= 0 || height <= 0) {
        return nullptr;
    }

    id<MTLCommandBuffer> cb     = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
    id<MTLTexture>       source = (__bridge id<MTLTexture>)sourceMtlTexture;

    // Resize / create the persistent output texture if needed.
    if (m_impl->outputTex == nil ||
        width != m_impl->outputW || height != m_impl->outputH) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:width
                                        height:height
                                     mipmapped:NO];
        td.usage       = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        m_impl->outputTex = [m_impl->device newTextureWithDescriptor:td];
        m_impl->outputW   = width;
        m_impl->outputH   = height;
    }
    if (!m_impl->outputTex) return nullptr;

    const Built &b = m_impl->active;
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:b.pipeline];
    [enc setTexture:source            atIndex:0];
    [enc setTexture:m_impl->outputTex atIndex:1];
    [enc setSamplerState:m_impl->sampler atIndex:0];
    if (b.minColor) {
        const MinColorGpu &g = m_impl->mcBlocks;
        [enc setBytes:&g.in    length:sizeof(g.in)    atIndex:0];
        [enc setBytes:&g.out   length:sizeof(g.out)   atIndex:1];
        [enc setBytes:&g.agx   length:sizeof(g.agx)   atIndex:2];
        [enc setBytes:&g.flags length:sizeof(g.flags) atIndex:3];
        // Viewer aids see the display encoding the Output half wrote.
        OutputEncoding enc_ = OutputEncoding::Sdr;
        switch (g.out.eotf) {
        case DRT_EOTF_PQ:     enc_ = OutputEncoding::Pq;     break;
        case DRT_EOTF_HLG:    enc_ = OutputEncoding::Hlg;    break;
        case DRT_EOTF_LINEAR: enc_ = OutputEncoding::Linear; break;
        default: break;
        }
        int primaries = 0;
        switch (g.out.display_gamut) {
        case DRT_DG_P3D65: case DRT_DG_P3D60: case DRT_DG_P3DCI: primaries = 1; break;
        case DRT_DG_REC2020_P3LIM: case DRT_DG_REC2020: case DRT_DG_XYZ:
        case DRT_DG_WORKING: case DRT_DG_AP0: case DRT_DG_AP1: primaries = 2; break;
        default: break;
        }
        const ViewerGpu viewer = linear_stage::resolveViewer(m_impl->viewer, enc_, primaries);
        [enc setBytes:&viewer length:sizeof(viewer) atIndex:4];
        const MTLSize tg   = MTLSizeMake(16, 16, 1);
        const MTLSize grid = MTLSizeMake(width, height, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        return (__bridge void *)m_impl->outputTex;
    }
    int texBind = 2;
    for (id<MTLTexture> lut : b.luts) {
        [enc setTexture:lut atIndex:texBind++];
    }
    if (b.split) {
        const LinearStageGpu stage =
            linear_stage::resolve(m_impl->stage, b.side, b.displayIsSdr);
        [enc setBytes:&stage length:sizeof(stage) atIndex:0];
    }
    const ViewerGpu viewer =
        linear_stage::resolveViewer(m_impl->viewer, b.encoding, b.outputPrimaries);
    [enc setBytes:&viewer length:sizeof(viewer) atIndex:1];

    const MTLSize tg   = MTLSizeMake(16, 16, 1);
    const MTLSize grid = MTLSizeMake(width, height, 1);
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    return (__bridge void *)m_impl->outputTex;
}

} // namespace qcv
