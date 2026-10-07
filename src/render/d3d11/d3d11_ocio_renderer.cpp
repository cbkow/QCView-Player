// D3D11OcioRenderer — see header for adaptation notes.

#include "d3d11_ocio_renderer.h"

#include "color/ocio_chain_builder.h"
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
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QtLogging>

#include <algorithm>
#include <deque>

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

// ---- minColor: the OCIO-free engine's pixel shader --------------------
// The vendored minColorAE core (color/mincolor/, see VENDORED.md) as text
// from the resource bundle, concatenated the way opendrt_shim_hlsl.h
// documents, after QCView's viewer aids. One shader, compiled once per
// process; the chain is four constant buffers (MinColorGpu):
//   b2 DrtParams in      Input half: file encoding → linear Rec.2020, knee fields
//   b3 DrtParams out     Output half: rendering + display encoding
//   b4 DrtAgxParams      AgX
//   b5 flags             knee on, AgX on, output scale (EDR)
//   b1 viewer aids       (kLinearStageHlsl; b0, the OCIO stage, is unused)
// Every block is 4-byte scalars, a multiple of 16 bytes, so the bytes the
// chain resolved upload as-is (opendrt_params.h, "WHY THIS SHAPE").
constexpr const char *kMinColorPsHlsl = R"(
cbuffer QcvMcInCb    : register(b2) { DrtParams    qcvMcIn;  };
cbuffer QcvMcOutCb   : register(b3) { DrtParams    qcvMcOut; };
// DrtAgxParams holds six float[9] matrices, and a cbuffer strides every
// array element to 16 bytes, so the struct cannot be declared over the
// bytes the chain resolved (the DrtParams blocks are scalars only and
// can). The block is read raw and the struct filled field by field;
// the loader below is generated from the header's field order.
cbuffer QcvMcAgxCb   : register(b4) { float4 qcvMcAgxRaw[21]; };
cbuffer QcvMcFlagsCb : register(b5) { int qcvMcKneeOn; int qcvMcAgxOn; float qcvMcOutScale; int qcvMcPad; };

float qcvMcAgxF(int i) { return qcvMcAgxRaw[i >> 2][i & 3]; }
DrtAgxParams qcvMcAgxLoad()
{
    DrtAgxParams a;
    a.working_gamut = qcvMcAgxF(0);
    a.target = qcvMcAgxF(1);
    a.peak = qcvMcAgxF(2);
    a.white_ev = qcvMcAgxF(3);
    a.black_ev = qcvMcAgxF(4);
    a.contrast = qcvMcAgxF(5);
    a.toe_power = qcvMcAgxF(6);
    a.shoulder_power = qcvMcAgxF(7);
    a.hue_restore = qcvMcAgxF(8);
    a.hdr_purity = qcvMcAgxF(9);
    a.outset = qcvMcAgxF(10);
    a.user_pad = qcvMcAgxF(11);
    [unroll] for (int k12 = 0; k12 < 9; ++k12) a.m_wb[k12] = qcvMcAgxF(12 + k12);
    [unroll] for (int k21 = 0; k21 < 9; ++k21) a.m_br[k21] = qcvMcAgxF(21 + k21);
    [unroll] for (int k30 = 0; k30 < 9; ++k30) a.m_rb[k30] = qcvMcAgxF(30 + k30);
    [unroll] for (int k39 = 0; k39 < 9; ++k39) a.m_bt[k39] = qcvMcAgxF(39 + k39);
    [unroll] for (int k48 = 0; k48 < 9; ++k48) a.m_tb[k48] = qcvMcAgxF(48 + k48);
    [unroll] for (int k57 = 0; k57 < 9; ++k57) a.m_bw[k57] = qcvMcAgxF(57 + k57);
    a.range = qcvMcAgxF(66);
    a.px = qcvMcAgxF(67);
    a.py = qcvMcAgxF(68);
    a.slope = qcvMcAgxF(69);
    a.toe_s = qcvMcAgxF(70);
    a.sh_s = qcvMcAgxF(71);
    a.sh_p = qcvMcAgxF(72);
    a.ratio = qcvMcAgxF(73);
    a.d_gx = qcvMcAgxF(74);
    a.d_gy = qcvMcAgxF(75);
    a.d_toe_s = qcvMcAgxF(76);
    a.d_sh_s = qcvMcAgxF(77);
    a.d_lo = qcvMcAgxF(78);
    a.d_range = qcvMcAgxF(79);
    a.l_t0 = qcvMcAgxF(80);
    a.l_t1 = qcvMcAgxF(81);
    a.l_t2 = qcvMcAgxF(82);
    a.guard = qcvMcAgxF(83);
    return a;
}

struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(VsOut input) : SV_TARGET
{
    float4 color = uSrc.Sample(uSrcSampler, input.uv);
    float3 c = color.rgb;
    c = drt_input_transform(qcvMcIn, c);
    if (qcvMcKneeOn != 0) c = drt_knee(qcvMcIn, c);
    if (qcvMcAgxOn != 0)  c = drt_agx(qcvMcAgxLoad(), c);
    c = drt_transform(qcvMcOut, c);
    color.rgb = c * qcvMcOutScale;
    return qcvViewerApply(color);
}
)";

std::string resourceText(const char *path, QString &error)
{
    QFile f(QString::fromLatin1(path));
    if (!f.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("D3D11OcioRenderer: missing kernel resource %1")
                    .arg(QString::fromLatin1(path));
        return {};
    }
    return f.readAll().toStdString();
}

std::string buildMinColorPsHlsl(QString &error)
{
    std::string s;
    s.reserve(96 * 1024);
    s += "Texture2D    uSrc        : register(t0);\n";
    s += "SamplerState uSrcSampler : register(s0);\n";
    s += kLinearStageHlsl;
    s += "\n";
    for (const char *part : {":/mincolor/opendrt_shim_hlsl.h", ":/mincolor/opendrt_params.h",
                             ":/mincolor/opendrt_kernel.h", ":/mincolor/mincolor_knee.h",
                             ":/mincolor/mincolor_agx.h"}) {
        const std::string text = resourceText(part, error);
        if (!error.isEmpty()) return {};
        s += "\n// ---- ";
        s += part;
        s += "\n";
        s += text;
    }
    s += kMinColorPsHlsl;
    return s;
}

// The compiled shader, shared by every instance on the device (live, B
// side, captures): D3DCompile of the core is seconds, and a capture
// instance must never pay it on the render thread. Built once, on a
// worker the first live instance starts at initialize(); a failure is
// final for the session.
struct MinColorKernel {
    std::mutex                mutex;
    std::condition_variable   done;
    ID3D11Device             *device = nullptr;
    ComPtr<ID3D11PixelShader> ps;
    QString                   error;
    bool                      tried   = false;
    bool                      running = false;
    std::thread               worker;

    ~MinColorKernel() { join(); }

    // Wait for a running compile and reap its thread.
    void join()
    {
        std::unique_lock lock(mutex);
        done.wait(lock, [this] { return !running; });
        if (worker.joinable()) worker.join();
    }
};

MinColorKernel &minColorKernel()
{
    static MinColorKernel k;
    return k;
}

// Compile on the calling thread (device methods only: thread-safe) and
// record the result.
void compileMinColorKernel(ID3D11Device *device)
{
    MinColorKernel &k = minColorKernel();
    QString err;
    const std::string src = buildMinColorPsHlsl(err);
    ComPtr<ID3D11PixelShader> ps;
    QElapsedTimer timer;
    timer.start();
    if (!src.empty()) {
        // Dumped before the compile, so a hang or a crash still leaves
        // the source to feed fxc by hand.
        const QString path = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                             + QStringLiteral("/qcv-mincolor.hlsl");
        if (QFile f(path); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(src.data(), static_cast<qint64>(src.size()));
            qInfo("D3D11OcioRenderer: minColor HLSL dumped to %s", qPrintable(path));
        }
    }
    // The bytecode, cached on disk by the source's hash: fxc takes ~12 s
    // on this kernel, CreatePixelShader from the blob is instant, so only
    // the first launch after a core or shim change pays the compile.
    QString cachePath;
    if (err.isEmpty()) {
        const QByteArray hash = QCryptographicHash::hash(
            QByteArray::fromRawData(src.data(), static_cast<qsizetype>(src.size())),
            QCryptographicHash::Sha1).toHex();
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                            + QStringLiteral("/shaders");
        QDir().mkpath(dir);
        cachePath = dir + QStringLiteral("/mincolor-ps50-") + QString::fromLatin1(hash)
                    + QStringLiteral(".cso");
    }
    bool fromCache = false;
    if (err.isEmpty()) {
        if (QFile f(cachePath); f.open(QIODevice::ReadOnly)) {
            const QByteArray blob = f.readAll();
            if (!blob.isEmpty()
                && SUCCEEDED(device->CreatePixelShader(blob.constData(),
                                                       static_cast<SIZE_T>(blob.size()), nullptr,
                                                       ps.GetAddressOf()))) {
                fromCache = true;
            } else {
                ps.Reset();
            }
        }
    }
    if (err.isEmpty() && !ps) {
        // IEEE strictness: the core is verified float for float against
        // the upstream DCTL, and Metal's fast math already showed what a
        // reassociating compiler does to its hue / purity functions (a
        // white rendered green). Same stance as fastMathEnabled = NO.
        ComPtr<ID3DBlob> blob = compileHlsl(src, "PSMain", "ps_5_0", &err,
                                            D3DCOMPILE_IEEE_STRICTNESS);
        if (blob) {
            if (FAILED(device->CreatePixelShader(blob->GetBufferPointer(),
                                                 blob->GetBufferSize(), nullptr,
                                                 ps.GetAddressOf()))) {
                err = QStringLiteral("D3D11OcioRenderer: CreatePixelShader failed (minColor)");
                ps.Reset();
            } else if (QFile f(cachePath); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(static_cast<const char *>(blob->GetBufferPointer()),
                        static_cast<qint64>(blob->GetBufferSize()));
            }
        }
    }
    const qint64 ms = timer.elapsed();
    if (ps) {
        qInfo("D3D11OcioRenderer: minColor kernel %s (%lld ms)",
              fromCache ? "loaded from cache" : "compiled", static_cast<long long>(ms));
    } else {
        qWarning("D3D11OcioRenderer: minColor kernel failed: %s", qPrintable(err));
    }
    {
        std::lock_guard lock(k.mutex);
        k.device  = device;
        k.ps      = ps;
        k.error   = err;
        k.tried   = true;
        k.running = false;
    }
    k.done.notify_all();
}

// Start the compile in the background unless it is done or under way.
void startMinColorKernel(ID3D11Device *device)
{
    MinColorKernel &k = minColorKernel();
    std::lock_guard lock(k.mutex);
    if (k.running || (k.tried && k.device == device)) return;
    if (k.worker.joinable()) k.worker.join();   // a finished earlier thread
    k.running = true;
    k.worker  = std::thread([device] { compileMinColorKernel(device); });
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

    OcioChainSpec lastSpec;               // the chain the active build (or its failure) is for
    bool    lastValid           = false;   // … false = nothing built yet / invalidated
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

    // minColor engine: `ps` is the shared kernel while activeMinColor;
    // the blocks are the chain, refreshed by rebuild() and uploaded by
    // apply() (b2..b5).
    bool                 activeMinColor = false;
    MinColorGpu          mcBlocks;
    ComPtr<ID3D11Buffer> mcInCb, mcOutCb, mcAgxCb, mcFlagsCb;

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
    OcioChainSpec              pendingSpec;

    // Built pipelines kept for reuse (most recent first) — prewarm()
    // fills it, so a chain change (a playlist cut, a clip switch) swaps
    // instantly. Failed keys are remembered so a broken chain isn't
    // rebuilt every frame. Render thread only.
    struct CacheEntry {
        OcioChainSpec             spec;
        bool                      split = false;
        ComPtr<ID3D11PixelShader> ps;
        std::vector<LutResource>  luts;
        bool                      builtSplit = false;
        InterchangeSide           side = InterchangeSide::None;
        bool                      displayIsSdr = true;
        OutputEncoding            encoding = OutputEncoding::Sdr;
        int                       outputPrimaries = 0;
    };
    std::deque<CacheEntry> cache;
    static constexpr size_t kCacheSize = 8;
    std::vector<std::pair<OcioChainSpec, bool>> failed;

    static bool sameKey(const OcioChainSpec &a, bool sa, const OcioChainSpec &b, bool sb)
    {
        return sa == sb && a.sameShader(b);
    }
    void remember(CacheEntry e)
    {
        if (!e.ps) {
            if (failed.size() >= 16) failed.erase(failed.begin());
            failed.emplace_back(e.spec, e.split);
            return;
        }
        cache.erase(std::remove_if(cache.begin(), cache.end(),
                                   [&](const CacheEntry &c) {
                                       return sameKey(c.spec, c.split, e.spec, e.split);
                                   }),
                    cache.end());
        cache.push_front(std::move(e));
        while (cache.size() > kCacheSize) cache.pop_back();
    }
    const CacheEntry *cached(const OcioChainSpec &spec, bool split) const
    {
        for (const CacheEntry &c : cache)
            if (sameKey(c.spec, c.split, spec, split)) return &c;
        return nullptr;
    }
    bool knownFailed(const OcioChainSpec &spec, bool split) const
    {
        for (const auto &f : failed)
            if (sameKey(f.first, f.second, spec, split)) return true;
        return false;
    }
    void installEntry(const CacheEntry &e)
    {
        ps              = e.ps;
        luts            = e.luts;
        lastSpec        = e.spec;
        lastValid       = true;
        lastSplitKey    = e.split;
        builtSplit      = e.builtSplit;
        side            = e.side;
        displayIsSdr    = e.displayIsSdr;
        encoding        = e.encoding;
        outputPrimaries = e.outputPrimaries;
        activeMinColor  = false;
        lastError.clear();
    }
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
    // minColor's blocks (b2..b5); every size is a multiple of 16.
    static_assert(sizeof(drt::DrtParams) % 16 == 0 && sizeof(drt::DrtAgxParams) == 84 * 4
                  && sizeof(MinColorFlagsGpu) == 16, "minColor blocks must be cbuffer-sized");
    struct { ComPtr<ID3D11Buffer> *buf; UINT size; } mc[] = {
        {&m_impl->mcInCb,    sizeof(drt::DrtParams)},
        {&m_impl->mcOutCb,   sizeof(drt::DrtParams)},
        {&m_impl->mcAgxCb,   sizeof(drt::DrtAgxParams)},
        {&m_impl->mcFlagsCb, sizeof(MinColorFlagsGpu)},
    };
    for (auto &b : mc) {
        cbd.ByteWidth = b.size;
        if (FAILED(m_impl->device->CreateBuffer(&cbd, nullptr, b.buf->GetAddressOf()))) {
            m_impl->lastError = QStringLiteral("D3D11OcioRenderer: minColor cbuffer create failed");
            return false;
        }
    }
    // The minColor kernel, ahead of its first use so an engine switch
    // never waits on a compile (plan: "switching and safety"). Capture
    // instances share whatever the live one built.
    if (!m_impl->sdrCapture.load()) startMinColorKernel(m_impl->device);
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
    // The shared minColor compile is not waited for: other instances
    // may still want it, and a renderer recreated mid-session must not
    // stall the render thread for a compile. The static's destructor
    // joins it at exit.
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
    m_impl->mcInCb.Reset();
    m_impl->mcOutCb.Reset();
    m_impl->mcAgxCb.Reset();
    m_impl->mcFlagsCb.Reset();
    m_impl->activeMinColor = false;
    m_impl->luts.clear();
    m_impl->device = nullptr;
    m_impl->lastValid = false;
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
    if (m_impl->activeMinColor && m_impl->ps) {
        if (m_impl->displayIsSdr) return 0.0f;
        const MinColorGpu &g = m_impl->mcBlocks;
        if (g.flags.kneeOn != 0) return g.in.kn_tgt;
        if (g.out.out_view == 0) return g.out.tn_Lp;   // OpenDRT renders to its peak
        return 0.0f;
    }
    if (!stageActive() || m_impl->displayIsSdr || !m_impl->stage.kneeEnabled) return 0.0f;
    return m_impl->stage.kneeTargetNits;
}

void D3D11OcioRenderer::setSdrCapture(bool on)
{
    m_impl->sdrCapture.store(on);
    m_impl->lastValid = false;
}

bool D3D11OcioRenderer::rebuild(const OcioChainSpec &spec)
{
    if (!spec.complete() || !isInitialized()) return false;

    if (spec.engine == ColorEngine::MinColor) {
        // The chain is constant buffers: take this frame's blocks, make
        // sure the shared kernel exists, and make it active. Until it is
        // compiled, keep drawing whatever is active (an OCIO chain or
        // nothing), as the Metal path does.
        Impl &i = *m_impl;
        i.mcBlocks = i.sdrCapture.load() ? spec.minColorSdr : spec.minColor;
        ComPtr<ID3D11PixelShader> ps;
        QString err;
        bool ready = false;
        {
            MinColorKernel &k = minColorKernel();
            std::lock_guard lock(k.mutex);
            if (k.tried && k.device == i.device) {
                ps    = k.ps;
                err   = k.error;
                ready = true;
            }
        }
        if (!ready) {
            startMinColorKernel(i.device);    // no-op while one is running
            return static_cast<bool>(i.ps);
        }
        if (!ps) {
            i.lastError = err;
            return false;
        }
        if (!(i.lastValid && i.activeMinColor)) {
            i.ps             = ps;
            i.luts.clear();
            i.lastSpec       = spec;
            i.lastValid      = true;
            i.lastSplitKey   = false;
            i.builtSplit     = false;
            i.side           = InterchangeSide::None;
            i.activeMinColor = true;
            i.lastError.clear();
            qInfo("D3D11OcioRenderer: minColor kernel active");
        }
        // Viewer aids (and the HDR10 peak) see the display encoding the
        // Output half writes.
        const drt::DrtParams &out = i.mcBlocks.out;
        switch (out.eotf) {
        case DRT_EOTF_PQ:     i.encoding = OutputEncoding::Pq;     break;
        case DRT_EOTF_HLG:    i.encoding = OutputEncoding::Hlg;    break;
        case DRT_EOTF_LINEAR: i.encoding = OutputEncoding::Linear; break;
        default:              i.encoding = OutputEncoding::Sdr;    break;
        }
        switch (out.display_gamut) {
        case DRT_DG_P3D65: case DRT_DG_P3D60: case DRT_DG_P3DCI: i.outputPrimaries = 1; break;
        case DRT_DG_REC2020_P3LIM: case DRT_DG_REC2020: case DRT_DG_XYZ:
        case DRT_DG_WORKING: case DRT_DG_AP0: case DRT_DG_AP1: i.outputPrimaries = 2; break;
        default: i.outputPrimaries = 0; break;
        }
        i.displayIsSdr = i.encoding == OutputEncoding::Sdr;
        return true;
    }

    const bool wantSplit = !linear_stage::isIdentity(m_impl->stage);

    // 1. Drain a finished worker (the live chain, or a prewarm): into the
    //    cache; installed when it is the chain asked for.
    {
        std::lock_guard lock(m_impl->swapMutex);
        if (m_impl->pendingReady) {
            Impl::CacheEntry e;
            e.spec            = m_impl->pendingSpec;
            e.split           = m_impl->pendingSplitKey;
            e.ps              = std::move(m_impl->pendingPs);
            e.luts            = std::move(m_impl->pendingLuts);
            e.builtSplit      = m_impl->pendingBuiltSplit;
            e.side            = m_impl->pendingSide;
            e.displayIsSdr    = m_impl->pendingDisplayIsSdr;
            e.encoding        = m_impl->pendingEncoding;
            e.outputPrimaries = m_impl->pendingPrimaries;
            if (Impl::sameKey(e.spec, e.split, spec, wantSplit)) {
                // A failure installs too (null ps): recorded against its
                // key so the next frame doesn't respawn the same compile.
                m_impl->installEntry(e);
                m_impl->lastError = std::move(m_impl->pendingError);
            }
            m_impl->remember(std::move(e));
            m_impl->pendingLuts.clear();
            m_impl->pendingError.clear();
            m_impl->pendingReady = false;
        }
    }

    // 2. Already on this chain? Trust whatever the last worker
    //    produced — success means ps is non-null, failure means we
    //    cached the failed chain here so the next frame doesn't
    //    re-spawn a worker compiling the same broken chain.
    if (m_impl->lastValid && m_impl->lastSpec.sameShader(spec)
        && wantSplit == m_impl->lastSplitKey) {
        return static_cast<bool>(m_impl->ps);
    }

    // 2b. Built ahead of time (prewarm) or earlier: swap it in now.
    if (const Impl::CacheEntry *hit = m_impl->cached(spec, wantSplit)) {
        m_impl->installEntry(*hit);
        qInfo("D3D11OcioRenderer: chain '%s' ready (built ahead)", qPrintable(spec.scene.input));
        return true;
    }

    // 3. Worker still running? Keep using the old pipeline (if any).
    //    Engine bypasses OCIO when this returns false (first-time
    //    compile) — user sees uncorrected source for a few seconds
    //    rather than dropping all frames.
    if (m_impl->rebuildInProgress.load()) {
        return static_cast<bool>(m_impl->ps);
    }

    // 4. Spawn a worker for the new chain. Join any previous thread
    //    object first (it's finished — rebuildInProgress would be
    //    false otherwise — just bookkeeping).
    if (m_impl->rebuildThread.joinable()) {
        m_impl->rebuildThread.join();
    }
    m_impl->rebuildInProgress.store(true);
    m_impl->rebuildThread = std::thread([this, spec, wantSplit]() {
        doRebuildWork(spec, wantSplit);
        m_impl->rebuildInProgress.store(false);
    });

    return static_cast<bool>(m_impl->ps);
}

void D3D11OcioRenderer::prewarm(const std::vector<OcioChainSpec> &specs, float gain)
{
    if (!isInitialized() || m_impl->sdrCapture.load()) return;
    if (m_impl->rebuildInProgress.load()) return;
    {
        std::lock_guard lock(m_impl->swapMutex);
        if (m_impl->pendingReady) return;   // rebuild() collects it first
    }
    for (const OcioChainSpec &spec : specs) {
        if (!spec.complete()) continue;
        const bool split = !linear_stage::isIdentity(spec.stage(gain));
        if (m_impl->lastValid && Impl::sameKey(m_impl->lastSpec, m_impl->lastSplitKey, spec, split))
            continue;
        if (m_impl->cached(spec, split) || m_impl->knownFailed(spec, split)) continue;
        if (m_impl->rebuildThread.joinable()) m_impl->rebuildThread.join();
        m_impl->rebuildInProgress.store(true);
        m_impl->rebuildThread = std::thread([this, spec, split]() {
            doRebuildWork(spec, split);
            m_impl->rebuildInProgress.store(false);
        });
        qInfo("D3D11OcioRenderer: building chain '%s' ahead of time",
              qPrintable(spec.scene.input));
        return;
    }
}

void D3D11OcioRenderer::doRebuildWork(const OcioChainSpec &spec, bool wantSplit)
{
    auto stageEmpty = [this, &spec, wantSplit](const QString &err) {
        {
            std::lock_guard lock(m_impl->swapMutex);
            m_impl->pendingPs.Reset();
            m_impl->pendingLuts.clear();
            m_impl->pendingSpec  = spec;
            m_impl->pendingSplitKey   = wantSplit;
            m_impl->pendingBuiltSplit = false;
            m_impl->pendingError = err;
            m_impl->pendingReady = true;
        }
        if (m_impl->wakeCallback) m_impl->wakeCallback();
    };

    // The spec carries its SDR capture Display/View.
    DisplayViewOverride sdr;
    const DisplayViewOverride *ov =
        m_impl->sdrCapture.load() ? DisplayViewOverride::sdrFor(spec, sdr) : nullptr;

    // Split chain for a non-identity linear stage. A chain that can't
    // split (no interchange role, data colourspace / view) runs unsplit
    // and the stage is skipped.
    OcioSplitChain split;
    if (wantSplit) {
        split = OcioChainBuilder::buildSplit(spec, OcioChainBuilder::Language::Hlsl_Sm_5_0, ov);
        if (!split.ok) {
            qInfo("D3D11OcioRenderer: stage unavailable (%s) — unsplit chain",
                  qPrintable(split.errorMessage));
        }
    }
    OcioChain chain;
    if (!split.ok) {
        chain = OcioChainBuilder::build(spec, OcioChainBuilder::Language::Hlsl_Sm_5_0, ov);
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
    qInfo("D3D11OcioRenderer: worker built chain '%s' (compile %lldms, %d 3D + %d 1D LUTs; "
          "%d slots by reflection, %d by fallback) — staging for render-thread swap",
          qPrintable(spec.scene.input), static_cast<long long>(compileMs), n3dKept, n1dKept,
          slotsByName, slotsFallback);

    // Stage the fresh pipeline + LUTs. The render thread will pick
    // them up on its next rebuild() call and atomically swap into
    // the active slot.
    {
        std::lock_guard lock(m_impl->swapMutex);
        m_impl->pendingPs    = newPs;
        m_impl->pendingLuts  = std::move(newLuts);
        m_impl->pendingSpec  = spec;
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

    auto upload = [ctx](ID3D11Buffer *cb, const void *data, size_t size) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (cb && SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, data, size);
            ctx->Unmap(cb, 0);
        }
    };

    // Bind by reflected slot — each LUT goes at the BindPoint D3D's
    // shader reflector reported for its name. Source stays at t0/s0
    // by convention (we declared it explicitly in buildPsHlsl).
    constexpr int kMaxSrvs = 16;
    ID3D11ShaderResourceView *srvs[kMaxSrvs] = {nullptr};
    ID3D11SamplerState       *smps[kMaxSrvs] = {nullptr};
    srvs[0] = srcSrv;
    smps[0] = m_impl->srcSampler.Get();
    int maxSlot = 0;
    if (!m_impl->activeMinColor) {
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
    }
    const UINT nBound = static_cast<UINT>(maxSlot + 1);
    ctx->PSSetShaderResources(0, nBound, srvs);
    ctx->PSSetSamplers       (0, nBound, smps);

    // minColor: the chain as constant buffers, refreshed every apply so
    // a slider move never rebuilds (b2..b5; the viewer at b1 below).
    if (m_impl->activeMinColor) {
        const MinColorGpu &g = m_impl->mcBlocks;
        upload(m_impl->mcInCb.Get(),    &g.in,    sizeof(g.in));
        upload(m_impl->mcOutCb.Get(),   &g.out,   sizeof(g.out));
        upload(m_impl->mcAgxCb.Get(),   &g.agx,   sizeof(g.agx));
        upload(m_impl->mcFlagsCb.Get(), &g.flags, sizeof(g.flags));
        ID3D11Buffer *cbs[4] = {m_impl->mcInCb.Get(), m_impl->mcOutCb.Get(),
                                m_impl->mcAgxCb.Get(), m_impl->mcFlagsCb.Get()};
        ctx->PSSetConstantBuffers(2, 4, cbs);
    }

    // Linear stage parameters (split pipelines only) — refreshed every
    // apply so slider moves need no rebuild.
    if (!m_impl->activeMinColor && m_impl->builtSplit && m_impl->stageCb) {
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
