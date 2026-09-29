// D3D11OcioRenderer — see header for adaptation notes.

#include "d3d11_ocio_renderer.h"

#include "color/ocio_chain_builder.h"
#include "color/ocio_config_manager.h"
#include "d3d11_device_manager.h"
#include "d3d11_ocio_luts.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <QElapsedTimer>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include <QFile>
#include <QStandardPaths>
#include <QtLogging>

#include <cstring>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

using Microsoft::WRL::ComPtr;

namespace {

// Fullscreen-triangle VS — same Y-flip as the F.1.a probe spike, so
// screen pixel (x, y) maps to texel (x, y).
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

// PS wrapper for OCIO. OCIO emits its own SamplerState + Texture*
// declarations inside its shader text (no register() qualifiers in 2.5 —
// the compiler assigns them and apply() binds by reflected name), so all
// we have to do is declare our source texture at t0/s0 and call
// OCIODisplay(src). The function takes (and returns) float4
// when OCIO is configured for 4-channel — the spike used the same form.
//
// Phase F.2.9: brightness adjustment lives in D3D11Compositor (pre-OCIO,
// linear-light). The OCIO PS is brightness-agnostic — it just runs
// OCIODisplay and writes the result. Post-OCIO scaling was prototyped
// (an SDR-reference-white knob to compensate for Windows' lower
// reference white vs macOS EDR) but rejected because it breaks the
// PQ-in → PQ-out 1:1 round-trip invariant. Linear-light exposure
// adjustment via the compositor's brightness slider is the right
// link in the chain: at gain=1.0 OCIO 1:1 holds; gain>1 lifts scene
// values pre-OCIO so the View transform re-encodes consistently.
std::string buildPsHlsl(const std::string &ocioFunction,
                          const std::string &funcName)
{
    std::string s;
    s.reserve(ocioFunction.size() + 512);
    s += "Texture2D    uSrc        : register(t0);\n";
    s += "SamplerState uSrcSampler : register(s0);\n";
    s += kLinearStageHlsl;     // viewer aids (cbuffer b1) + PQ helpers
    s += "\n";
    s += ocioFunction;
    s += "\n\nstruct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n";
    s += "float4 PSMain(VsOut input) : SV_TARGET\n";
    s += "{\n";
    s += "    float4 src = uSrc.Sample(uSrcSampler, input.uv);\n";
    s += "    return qcvViewerApply(" + funcName + "(src));\n";
    s += "}\n";
    return s;
}

// Split chain: the linear stage (cbuffer b0) between OCIOPre and
// OCIOPost — see color/linear_stage.h. The two OCIO functions carry
// distinct resource prefixes; LUT slots are resolved by reflection.
std::string buildSplitPsHlsl(const std::string &preFunction,
                             const std::string &postFunction)
{
    std::string s;
    s.reserve(preFunction.size() + postFunction.size() + 4096);
    s += "Texture2D    uSrc        : register(t0);\n";
    s += "SamplerState uSrcSampler : register(s0);\n";
    s += kLinearStageHlsl;
    s += "\n";
    s += preFunction;
    s += "\n";
    s += postFunction;
    s += "\n\nstruct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n";
    s += "float4 PSMain(VsOut input) : SV_TARGET\n";
    s += "{\n";
    s += "    float4 c = uSrc.Sample(uSrcSampler, input.uv);\n";
    s += "    c = OCIOPre(c);\n";
    s += "    c = qcvLinearStage(c);\n";
    s += "    return qcvViewerApply(OCIOPost(c));\n";
    s += "}\n";
    return s;
}

} // namespace

struct D3D11OcioRenderer::Impl {
    ID3D11Device        *device  = nullptr;
    ComPtr<ID3D11VertexShader>  vs;
    ComPtr<ID3D11PixelShader>   ps;
    ComPtr<ID3D11SamplerState>  srcSampler;

    // Active LUTs in OCIO's declaration order (3D first, then 1D/2D).
    // Slot positions inside each LutResource are reflection-resolved
    // by OCIO's texture/sampler name, mirroring how MetalOcioRenderer
    // binds via MSL function parameter names.
    std::vector<LutResource> luts;

    int     lastChainGeneration = -1;
    bool    lastSplitKey        = false;   // whether the active build was asked to split
    QString lastError;

    // Linear stage: the settings for the next rebuild()/apply(), and what
    // the active pipeline was actually built as.
    LinearStageSettings  stage;
    bool                 builtSplit   = false;
    InterchangeSide      side         = InterchangeSide::None;
    bool                 displayIsSdr = true;
    OutputEncoding       encoding     = OutputEncoding::Sdr;
    int                  outputPrimaries = 0;
    ComPtr<ID3D11Buffer> stageCb;             // LinearStageGpu at b0
    ViewerAids           viewer;
    ComPtr<ID3D11Buffer> viewerCb;            // ViewerGpu at b1
    std::atomic<bool> sdrCapture{false};   // read by the rebuild worker

    // --- Async rebuild plumbing -------------------------------------
    // D3DCompile on heavy OCIO chains (AgX with multi-hundred-line
    // helper functions + large static arrays) can take 5-15 seconds
    // even at OPTIMIZATION_LEVEL_1. Doing it on the render thread
    // freezes playback. We compile + create LUT resources on a
    // worker thread, stage the result under `swapMutex`, and the
    // render thread swaps the active pipeline in on its next rebuild()
    // call. While the worker is busy, rebuild() returns false so
    // drawFrame() bypasses OCIO and shows uncorrected source — better
    // than dropping all frames during compile.
    std::atomic<bool>          rebuildInProgress{false};
    std::thread                rebuildThread;

    std::mutex                 swapMutex;       // guards the four pending* fields
    bool                       pendingReady = false;
    ComPtr<ID3D11PixelShader>  pendingPs;
    std::vector<LutResource>   pendingLuts;
    int                        pendingGen   = -1;
    bool                       pendingSplitKey     = false;
    bool                       pendingBuiltSplit   = false;
    InterchangeSide            pendingSide         = InterchangeSide::None;
    bool                       pendingDisplayIsSdr = true;
    OutputEncoding             pendingEncoding     = OutputEncoding::Sdr;
    int                        pendingPrimaries    = 0;
    QString                    pendingError;

    // Render-thread wake callback fired from the worker when a fresh
    // pipeline has been staged. Lets a paused-playback view switch
    // pick up the new chain without waiting for the next decoded
    // frame. Held by value (copyable std::function) so the worker
    // calling it doesn't race with renderer teardown after we join
    // the thread in shutdown().
    std::function<void()>      wakeCallback;
};

D3D11OcioRenderer::D3D11OcioRenderer()
    : m_impl(std::make_unique<Impl>()) {}

D3D11OcioRenderer::~D3D11OcioRenderer()
{
    shutdown();
}

bool D3D11OcioRenderer::initialize()
{
    auto &mgr = D3D11DeviceManager::instance();
    m_impl->device = static_cast<ID3D11Device *>(mgr.device());
    if (!m_impl->device) {
        qWarning("D3D11OcioRenderer: no D3D11 device");
        return false;
    }

    QString vsErr;
    ComPtr<ID3DBlob> vsBlob = compileHlsl(kVsHlsl, "VSMain", "vs_5_0", &vsErr);
    if (!vsBlob) {
        m_impl->lastError = vsErr;
        qWarning("%s", qPrintable(m_impl->lastError));
        return false;
    }
    if (FAILED(m_impl->device->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            nullptr, m_impl->vs.GetAddressOf()))) {
        m_impl->lastError = QStringLiteral("D3D11OcioRenderer: CreateVertexShader failed");
        return false;
    }

    m_impl->srcSampler = makeLinearClampSampler(m_impl->device);
    if (!m_impl->srcSampler) {
        m_impl->lastError = QStringLiteral("D3D11OcioRenderer: srcSampler create failed");
        return false;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(LinearStageGpu);   // 128, a multiple of 16
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_impl->device->CreateBuffer(&cbd, nullptr, m_impl->stageCb.GetAddressOf()))) {
        m_impl->lastError = QStringLiteral("D3D11OcioRenderer: stage cbuffer create failed");
        return false;
    }
    cbd.ByteWidth = sizeof(ViewerGpu);                 // 32
    if (FAILED(m_impl->device->CreateBuffer(&cbd, nullptr, m_impl->viewerCb.GetAddressOf()))) {
        m_impl->lastError = QStringLiteral("D3D11OcioRenderer: viewer cbuffer create failed");
        return false;
    }
    return true;
}

void D3D11OcioRenderer::shutdown()
{
    if (!m_impl) return;
    // Wait for any in-flight worker so it doesn't keep referencing
    // the device after teardown. D3DCompile can't be cancelled, so
    // this may block up to a few seconds.
    if (m_impl->rebuildThread.joinable()) {
        m_impl->rebuildThread.join();
    }
    {
        std::lock_guard lock(m_impl->swapMutex);
        m_impl->pendingPs.Reset();
        m_impl->pendingLuts.clear();
        m_impl->pendingReady = false;
    }
    m_impl->vs.Reset();
    m_impl->ps.Reset();
    m_impl->srcSampler.Reset();
    m_impl->stageCb.Reset();
    m_impl->viewerCb.Reset();
    m_impl->luts.clear();
    m_impl->device = nullptr;
    m_impl->lastChainGeneration = -1;
}

bool D3D11OcioRenderer::isInitialized() const
{
    return m_impl && m_impl->device != nullptr && m_impl->vs && m_impl->srcSampler;
}

bool D3D11OcioRenderer::hasPipeline() const
{
    return m_impl && m_impl->ps;
}

const QString &D3D11OcioRenderer::lastError() const
{
    return m_impl->lastError;
}

void D3D11OcioRenderer::setWakeCallback(std::function<void()> cb)
{
    m_impl->wakeCallback = std::move(cb);
}

void D3D11OcioRenderer::setViewer(const ViewerAids &viewer)
{
    m_impl->viewer = viewer;
}

void D3D11OcioRenderer::setStage(const LinearStageSettings &stage)
{
    m_impl->stage = stage;
}

bool D3D11OcioRenderer::stageActive() const
{
    return m_impl && m_impl->ps && m_impl->builtSplit;
}

float D3D11OcioRenderer::hdrKneeTargetNits() const
{
    if (!stageActive() || m_impl->displayIsSdr || !m_impl->stage.kneeEnabled) return 0.0f;
    return m_impl->stage.kneeTargetNits;
}

void D3D11OcioRenderer::setSdrCapture(bool on)
{
    m_impl->sdrCapture.store(on);
    m_impl->lastChainGeneration = -1;
}

bool D3D11OcioRenderer::rebuild(OCIOConfigManager *ocio)
{
    if (!ocio || !isInitialized()) return false;

    const int  gen       = ocio->activeChainGeneration();
    const bool wantSplit = !linear_stage::isIdentity(m_impl->stage);

    // 1. Drain any pending result from a previously-finished worker.
    {
        std::lock_guard lock(m_impl->swapMutex);
        if (m_impl->pendingReady) {
            m_impl->ps                  = std::move(m_impl->pendingPs);
            m_impl->luts                = std::move(m_impl->pendingLuts);
            m_impl->lastChainGeneration = m_impl->pendingGen;
            m_impl->lastSplitKey        = m_impl->pendingSplitKey;
            m_impl->builtSplit          = m_impl->pendingBuiltSplit;
            m_impl->side                = m_impl->pendingSide;
            m_impl->displayIsSdr        = m_impl->pendingDisplayIsSdr;
            m_impl->encoding            = m_impl->pendingEncoding;
            m_impl->outputPrimaries     = m_impl->pendingPrimaries;
            m_impl->lastError           = std::move(m_impl->pendingError);
            m_impl->pendingPs.Reset();
            m_impl->pendingLuts.clear();
            m_impl->pendingError.clear();
            m_impl->pendingReady = false;
        }
    }

    // 2. Already at the right gen? Trust whatever the last worker
    //    produced — success means ps is non-null, failure means we
    //    cached the failed gen here so the next frame doesn't
    //    re-spawn a worker compiling the same broken chain.
    if (gen == m_impl->lastChainGeneration && wantSplit == m_impl->lastSplitKey) {
        return static_cast<bool>(m_impl->ps);
    }

    // 3. Worker still running? Keep using the old pipeline (if any).
    //    Engine bypasses OCIO when this returns false (first-time
    //    compile) — user sees uncorrected source for a few seconds
    //    rather than dropping all frames.
    if (m_impl->rebuildInProgress.load()) {
        return static_cast<bool>(m_impl->ps);
    }

    // 4. Spawn a worker for the new gen. Join any previous thread
    //    object first (it's finished — rebuildInProgress would be
    //    false otherwise — just bookkeeping).
    if (m_impl->rebuildThread.joinable()) {
        m_impl->rebuildThread.join();
    }
    m_impl->rebuildInProgress.store(true);
    m_impl->rebuildThread = std::thread([this, gen, wantSplit, ocio]() {
        doRebuildWork(gen, wantSplit, ocio);
        m_impl->rebuildInProgress.store(false);
    });

    return static_cast<bool>(m_impl->ps);
}

void D3D11OcioRenderer::doRebuildWork(int gen, bool wantSplit, OCIOConfigManager *ocio)
{
    auto stageEmpty = [this, gen, wantSplit](const QString &err) {
        {
            std::lock_guard lock(m_impl->swapMutex);
            m_impl->pendingPs.Reset();
            m_impl->pendingLuts.clear();
            m_impl->pendingGen   = gen;
            m_impl->pendingSplitKey   = wantSplit;
            m_impl->pendingBuiltSplit = false;
            m_impl->pendingError = err;
            m_impl->pendingReady = true;
        }
        if (m_impl->wakeCallback) m_impl->wakeCallback();
    };

    // The SDR mapping is a pure function of the active chain, so the
    // generation cache stays valid for the capture instance too.
    DisplayViewOverride sdr;
    const bool useSdr = m_impl->sdrCapture.load()
        && ocio->sdrCaptureDisplayView(&sdr.display, &sdr.view);
    const DisplayViewOverride *ov = useSdr ? &sdr : nullptr;

    // Split chain for a non-identity linear stage. A chain that can't
    // split (no interchange role, data colourspace / view) runs unsplit
    // and the stage is skipped.
    OcioSplitChain split;
    if (wantSplit) {
        split = OcioChainBuilder::buildSplit(ocio, OcioChainBuilder::Language::Hlsl_Sm_5_0, ov);
        if (!split.ok) {
            qInfo("D3D11OcioRenderer: stage unavailable (%s) — unsplit chain",
                  qPrintable(split.errorMessage));
        }
    }
    OcioChain chain;
    if (!split.ok) {
        chain = OcioChainBuilder::build(ocio, OcioChainBuilder::Language::Hlsl_Sm_5_0, ov);
        if (!chain.ok) {
            stageEmpty(chain.errorMessage);
            return;
        }
    }

    std::vector<LutResource> newLuts;
    QString lutErr;
    std::string psSource;
    if (split.ok) {
        if (!createLuts(m_impl->device, split.pre.desc, newLuts, lutErr) ||
            !createLuts(m_impl->device, split.post.desc, newLuts, lutErr)) {
            stageEmpty(lutErr);
            return;
        }
        psSource = buildSplitPsHlsl(split.pre.shaderText.toStdString(),
                                    split.post.shaderText.toStdString());
    } else {
        if (!createLuts(m_impl->device, chain.desc, newLuts, lutErr)) {
            stageEmpty(lutErr);
            return;
        }
        psSource = buildPsHlsl(chain.shaderText.toStdString(), "OCIODisplay");
    }

    // Compile PS combining the OCIO function(s) + thin wrapper.
    QElapsedTimer compileTimer;
    compileTimer.start();
    QString psErr;
    ComPtr<ID3DBlob> psBlob = compileHlsl(psSource, "PSMain", "ps_5_0", &psErr);
    const qint64 compileMs = compileTimer.elapsed();
    if (!psBlob) {
        qWarning("%s", qPrintable(psErr));
        // Dump for inspection.
        const QString tmpDir =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString dumpPath = tmpDir + QStringLiteral("/qcv-ocio-hlsl-fail.hlsl");
        if (QFile f(dumpPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(psSource.data(), psSource.size());
            qInfo("D3D11OcioRenderer: dumped failing HLSL to %s",
                  qPrintable(dumpPath));
        }
        stageEmpty(psErr);
        return;
    }
    ComPtr<ID3D11PixelShader> newPs;
    if (FAILED(m_impl->device->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
            nullptr, newPs.GetAddressOf()))) {
        stageEmpty(QStringLiteral("D3D11OcioRenderer: CreatePixelShader failed"));
        return;
    }

    // Reflect the compiled PS to discover each LUT's bind slot by
    // name — matches the macOS Metal pattern (which names function
    // parameters) rather than relying on iteration-order matching
    // OCIO's emitted `: register(tN)` qualifiers. If a LUT can't be
    // found by name (some OCIO emitters rename or drop unused LUTs
    // during shader-text emission), we fall back to the iteration-
    // order slot the F.1.a spike was proven to work with.
    int slotsByName = 0, slotsFallback = 0;
    resolveLutSlots(psBlob.Get(), newLuts, slotsByName, slotsFallback);

    // Dump the OCIO HLSL + the wrapped PS source on every rebuild —
    // overwrites each time, so the file always reflects the latest
    // chain. Useful for diagnosing "wrong colors / black output" by
    // hand-inspecting which registers OCIO actually assigned.
    {
        const QString tmpDir =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString hlslPath = tmpDir + QStringLiteral("/qcv-ocio.hlsl");
        if (QFile f(hlslPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(psSource.data(), psSource.size());
        }
        qInfo("D3D11OcioRenderer: HLSL dumped to %s", qPrintable(hlslPath));
    }
    for (const auto &lut : newLuts) {
        qInfo("  LUT %s (%s) → tex t%d, smp s%d",
              lut.texName.c_str(),
              lut.is3d ? "3D" : lut.dimLabel,
              lut.texSlot, lut.smpSlot);
    }

    int n3dKept = 0, n1dKept = 0;
    for (const auto &l : newLuts) (l.is3d ? n3dKept : n1dKept)++;
    qInfo("D3D11OcioRenderer: worker built gen %d (compile %lldms, %d 3D + %d 1D LUTs; "
          "%d slots by reflection, %d by fallback) — staging for render-thread swap",
          gen, static_cast<long long>(compileMs), n3dKept, n1dKept,
          slotsByName, slotsFallback);

    // Stage the fresh pipeline + LUTs. The render thread will pick
    // them up on its next rebuild() call and atomically swap into
    // the active slot.
    {
        std::lock_guard lock(m_impl->swapMutex);
        m_impl->pendingPs    = newPs;
        m_impl->pendingLuts  = std::move(newLuts);
        m_impl->pendingGen   = gen;
        m_impl->pendingSplitKey     = wantSplit;
        m_impl->pendingBuiltSplit   = split.ok;
        m_impl->pendingSide         = split.side;
        m_impl->pendingDisplayIsSdr = split.displayIsSdr;
        m_impl->pendingEncoding     = split.ok ? split.encoding : chain.encoding;
        m_impl->pendingPrimaries    = split.ok ? split.outputPrimaries : chain.outputPrimaries;
        m_impl->pendingError.clear();
        m_impl->pendingReady = true;
    }
    // Wake the render thread (it may be in a paused-playback wait
    // with no decoded frames to nudge it otherwise).
    if (m_impl->wakeCallback) m_impl->wakeCallback();
}

void D3D11OcioRenderer::apply(void *ctxPtr,
                                 void *srcSrvPtr,
                                 void *dstRtvPtr,
                                 int   dstW, int dstH)
{
    if (!hasPipeline() || !ctxPtr || !srcSrvPtr || !dstRtvPtr ||
        dstW <= 0 || dstH <= 0) {
        return;
    }
    auto *ctx     = static_cast<ID3D11DeviceContext *>(ctxPtr);
    auto *srcSrv  = static_cast<ID3D11ShaderResourceView *>(srcSrvPtr);
    auto *dstRtv  = static_cast<ID3D11RenderTargetView *>(dstRtvPtr);

    ctx->OMSetRenderTargets(1, &dstRtv, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(dstW);
    vp.Height   = static_cast<float>(dstH);
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ctx->RSSetViewports(1, &vp);

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_impl->vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_impl->ps.Get(), nullptr, 0);

    // Bind by reflected slot — each LUT goes at the BindPoint D3D's
    // shader reflector reported for its name. Source stays at t0/s0
    // by convention (we declared it explicitly in buildPsHlsl).
    constexpr int kMaxSrvs = 16;
    ID3D11ShaderResourceView *srvs[kMaxSrvs] = {nullptr};
    ID3D11SamplerState       *smps[kMaxSrvs] = {nullptr};
    srvs[0] = srcSrv;
    smps[0] = m_impl->srcSampler.Get();
    int maxSlot = 0;
    for (const auto &lut : m_impl->luts) {
        if (lut.texSlot >= 0 && lut.texSlot < kMaxSrvs) {
            srvs[lut.texSlot] = lut.srv.Get();
            if (lut.texSlot > maxSlot) maxSlot = lut.texSlot;
        }
        if (lut.smpSlot >= 0 && lut.smpSlot < kMaxSrvs) {
            smps[lut.smpSlot] = lut.sampler.Get();
            if (lut.smpSlot > maxSlot) maxSlot = lut.smpSlot;
        }
    }
    const UINT nBound = static_cast<UINT>(maxSlot + 1);
    ctx->PSSetShaderResources(0, nBound, srvs);
    ctx->PSSetSamplers       (0, nBound, smps);

    // Linear stage parameters (split pipelines only) — refreshed every
    // apply so slider moves need no rebuild.
    if (m_impl->builtSplit && m_impl->stageCb) {
        const LinearStageGpu stage =
            linear_stage::resolve(m_impl->stage, m_impl->side, m_impl->displayIsSdr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(m_impl->stageCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &stage, sizeof(stage));
            ctx->Unmap(m_impl->stageCb.Get(), 0);
        }
        ID3D11Buffer *cb = m_impl->stageCb.Get();
        ctx->PSSetConstantBuffers(0, 1, &cb);
    }
    if (m_impl->viewerCb) {
        const ViewerGpu viewer = linear_stage::resolveViewer(
            m_impl->viewer, m_impl->encoding, m_impl->outputPrimaries);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(m_impl->viewerCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &viewer, sizeof(viewer));
            ctx->Unmap(m_impl->viewerCb.Get(), 0);
        }
        ID3D11Buffer *cb = m_impl->viewerCb.Get();
        ctx->PSSetConstantBuffers(1, 1, &cb);
    }

    ctx->Draw(3, 0);

    // Unbind the source SRV slot so the next pass (or a subsequent
    // CopyResource into the same texture) doesn't trip D3D11's
    // "resource still bound" debug-layer warning.
    ID3D11ShaderResourceView *nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
}

} // namespace qcv
