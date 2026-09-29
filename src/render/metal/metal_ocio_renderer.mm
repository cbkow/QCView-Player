// MetalOcioRenderer — see header for adaptation notes.

#include "metal_ocio_renderer.h"

#include "color/ocio_chain_builder.h"
#include "color/ocio_config_manager.h"
#include "metal_device_manager.h"
#include "metal_ocio_luts.h"

#import <Metal/Metal.h>

#include <QFile>
#include <QtLogging>

#include <algorithm>
#include <cstring>
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
                                          const QString &ocioText, QString &error)
{
    NSError *err = nil;
    NSString *src = [NSString stringWithUTF8String:kernel.toUtf8().constData()];
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        error = QStringLiteral("MetalOcioRenderer: kernel compile failed: %1")
            .arg(err ? QString::fromUtf8([err.localizedDescription UTF8String])
                     : QStringLiteral("(no error)"));
        qWarning("%s", qPrintable(error));
        dumpFailure(kernel, ocioText);
        return nil;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:@"ocio_apply"];
    if (!fn) {
        error = QStringLiteral("MetalOcioRenderer: ocio_apply not found");
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

// Build a pipeline for the active chain. Pure function of its inputs —
// safe on a background queue (MTLDevice is thread-safe; the OCIO reads
// mirror what the D3D11 compile worker already does).
Built buildPipeline(id<MTLDevice> device, OCIOConfigManager *ocio,
                    bool sdrCapture, bool wantSplit)
{
    Built out;
    // The SDR mapping is a pure function of the active chain, so the
    // generation cache stays valid for the capture instance too.
    DisplayViewOverride sdr;
    const bool useSdr = sdrCapture && ocio->sdrCaptureDisplayView(&sdr.display, &sdr.view);
    const DisplayViewOverride *ov = useSdr ? &sdr : nullptr;

    if (wantSplit) {
        OcioSplitChain chain =
            OcioChainBuilder::buildSplit(ocio, OcioChainBuilder::Language::Msl_2_0, ov);
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

    OcioChain chain = OcioChainBuilder::build(ocio, OcioChainBuilder::Language::Msl_2_0, ov);
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

struct MetalOcioRenderer::Impl {
    id<MTLDevice>       device  = nil;
    id<MTLSamplerState> sampler = nil;

    Built active;
    int   activeGen   = -1;   // chain generation the active build (or its failure) is for
    bool  activeSplit = false; // ... and whether it was asked to split

    // Background compile (async instances). One job at a time; the
    // result waits in `pending` until the render thread swaps it in.
    dispatch_queue_t queue = nullptr;
    std::mutex       pendingMutex;
    std::unique_ptr<Built> pending;
    int   pendingGen   = -1;
    bool  pendingSplit = false;
    bool  jobRunning   = false;
    int   jobGen       = -1;
    bool  jobSplit     = false;

    LinearStageSettings stage;
    ViewerAids          viewer;
    bool  async      = false;
    bool  sdrCapture = false;

    id<MTLTexture> outputTex = nil;
    int            outputW   = 0;
    int            outputH   = 0;

    QString lastError;

    void install(Built &&b, int gen, bool split)
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
        activeGen   = gen;
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
    m_impl->active    = Built{};
    m_impl->activeGen = -1;
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
    m_impl->sdrCapture = on;
    m_impl->activeGen  = -1;
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

bool MetalOcioRenderer::rebuild(OCIOConfigManager *ocio)
{
    if (!ocio || !isInitialized()) return false;
    Impl &i = *m_impl;

    const int  gen       = ocio->activeChainGeneration();
    const bool wantSplit = !linear_stage::isIdentity(i.stage);

    // 1. A finished background build waiting to be swapped in?
    if (i.async) {
        std::unique_ptr<Built> ready;
        int readyGen = -1;
        bool readySplit = false;
        {
            std::lock_guard lock(i.pendingMutex);
            if (i.pending) {
                ready = std::move(i.pending);
                readyGen = i.pendingGen;
                readySplit = i.pendingSplit;
            }
        }
        if (ready) {
            i.install(std::move(*ready), readyGen, readySplit);
            if (i.active.pipeline) {
                qInfo("MetalOcioRenderer: swapped in chain gen %d (%s, %zu LUTs)",
                      readyGen, i.active.split ? "split" : "unsplit", i.active.luts.size());
            }
        }
    }

    // 2. Already current?
    if (gen == i.activeGen && wantSplit == i.activeSplit) {
        return i.active.pipeline != nil;
    }

    // 3. Synchronous build: capture instances, and the first build of a
    //    session (nothing to keep showing meanwhile).
    if (!i.async || !i.active.pipeline) {
        Built b = buildPipeline(i.device, ocio, i.sdrCapture, wantSplit);
        i.install(std::move(b), gen, wantSplit);
        if (i.active.pipeline) {
            qInfo("MetalOcioRenderer: rebuilt for chain gen %d (%s, %zu LUTs)",
                  gen, i.active.split ? "split" : "unsplit", i.active.luts.size());
        }
        return i.active.pipeline != nil;
    }

    // 4. Background build; keep the current pipeline until it lands.
    {
        std::lock_guard lock(i.pendingMutex);
        if (i.jobRunning && i.jobGen == gen && i.jobSplit == wantSplit) {
            return true;   // already compiling this key
        }
        if (i.jobRunning) {
            return true;   // one job at a time; re-evaluated when it lands
        }
        i.jobRunning = true;
        i.jobGen     = gen;
        i.jobSplit   = wantSplit;
    }
    id<MTLDevice> device = i.device;
    const bool sdrCapture = i.sdrCapture;
    Impl *impl = m_impl;
    dispatch_async(i.queue, ^{
        auto b = std::make_unique<Built>(buildPipeline(device, ocio, sdrCapture, wantSplit));
        std::lock_guard lock(impl->pendingMutex);
        impl->pending      = std::move(b);
        impl->pendingGen   = gen;
        impl->pendingSplit = wantSplit;
        impl->jobRunning   = false;
    });
    return true;
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
