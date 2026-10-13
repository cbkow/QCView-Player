// MfVideoEncoder — see mf_video_encoder.h. Shape and settings mirror the
// VideoToolbox session (vt_video_encoder.mm) and the QCBridge Windows
// capture helper's MFT setup (agent/capture-win, HEVC there, H.264 here):
// async hardware MFT, D3DManager, CBR, no B-frames, GOP = one second,
// low latency, 709 tags, limited range.

#include "mf_video_encoder.h"

#include "render/d3d11/d3d11_device_manager.h"

#include <initguid.h>   // CODECAPI_* storage in this TU (codecapi.h is DEFINE_GUID)
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <strmif.h>     // ICodecAPI
#include <codecapi.h>
#include <wrl/client.h>

#include <QtLogging>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace qcv::output {

namespace {

std::string hrText(HRESULT hr)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

VARIANT varU32(uint32_t v) { VARIANT r; VariantInit(&r); r.vt = VT_UI4; r.ulVal = v; return r; }
VARIANT varBool(bool v)    { VARIANT r; VariantInit(&r); r.vt = VT_BOOL; r.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE; return r; }

constexpr int64_t kHnsPerSec = 10000000;   // Media Foundation time: 100 ns units

int64_t to100ns(int64_t t90k) { return (t90k * kHnsPerSec + 45000) / 90000; }

// NAL unit type of the unit starting at p (after its start code).
int nalType(const uint8_t *p) { return p[0] & 0x1f; }

} // namespace

// IMFAsyncCallback receiving the MFT's events on a multithreaded work queue.
class MfEventCallback final : public IMFAsyncCallback {
public:
    explicit MfEventCallback(MfVideoEncoder::Impl *impl) : m_impl(impl) {}
    void detach() { std::lock_guard<std::mutex> lk(m_mtx); m_impl = nullptr; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --m_refs;
        if (n == 0) delete this;
        return n;
    }
    // IMFAsyncCallback
    STDMETHODIMP GetParameters(DWORD *flags, DWORD *queue) override
    {
        if (flags) *flags = 0;
        if (queue) *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
        return S_OK;
    }
    STDMETHODIMP Invoke(IMFAsyncResult *result) override;

private:
    std::mutex m_mtx;
    MfVideoEncoder::Impl *m_impl;
    std::atomic<ULONG> m_refs{1};
};

struct MfVideoEncoder::Impl {
    // ---- surfaces (pool), guarded by poolMutex ----
    struct Surface { ComPtr<ID3D11Texture2D> tex; int refs = 0; };
    std::mutex poolMutex;
    std::vector<std::unique_ptr<Surface>> pool;
    uint32_t poolW = 0, poolH = 0;
    static constexpr size_t kPoolCap = 8;

    // ---- the session ----
    ComPtr<IMFTransform>           mft;
    ComPtr<IMFMediaEventGenerator> gen;
    ComPtr<ICodecAPI>              codec;
    ComPtr<IMFDXGIDeviceManager>   mgr;
    UINT  resetToken = 0;
    DWORD inId = 0, outId = 0;
    bool  providesSamples = true;
    DWORD outSize = 0;
    MfEventCallback *cb = nullptr;      // COM-owned; detach() in close()
    std::string name = "Media Foundation H.264";
    bool mfStarted = false;

    Callback    userCb;
    EncoderSpec spec;
    std::atomic<bool> open{false};
    std::atomic<uint64_t> submitted{0}, encoded{0}, keyframes{0}, bytes{0}, errors{0};

    // ---- input queue and the frames inside the MFT (FIFO: no reordering) ----
    struct Pending { ID3D11Texture2D *tex; int64_t pts90k, dur90k; bool key; };
    std::mutex              mtx;
    std::deque<Pending>     queue;        // waiting for a NeedInput
    int                     needInput = 0;
    std::deque<Pending>     inFlight;     // handed to the MFT, output not yet seen
    std::vector<uint8_t>    sps, pps;     // the stream's parameter sets, as last seen
    std::condition_variable drainCv;
    bool                    drained = false;
    bool                    streaming = false;

    ID3D11Device *device() const
    {
        return static_cast<ID3D11Device *>(D3D11DeviceManager::instance().device());
    }

    // --- pool ---
    void *acquire()
    {
        std::lock_guard<std::mutex> lk(poolMutex);
        for (auto &s : pool) if (s->refs == 0) { s->refs = 1; return s->tex.Get(); }
        if (pool.size() >= kPoolCap || !poolW || !poolH) return nullptr;
        ID3D11Device *dev = device();
        if (!dev) return nullptr;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = poolW;  td.Height = poolH;
        td.MipLevels = 1;  td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto s = std::make_unique<Surface>();
        const HRESULT hr = dev->CreateTexture2D(&td, nullptr, s->tex.GetAddressOf());
        if (FAILED(hr)) {
            qWarning("MfVideoEncoder: NV12 surface %ux%u failed (%s)", poolW, poolH, hrText(hr).c_str());
            return nullptr;
        }
        s->refs = 1;
        pool.push_back(std::move(s));
        return pool.back()->tex.Get();
    }
    void retain(void *surface)
    {
        std::lock_guard<std::mutex> lk(poolMutex);
        for (auto &s : pool) if (s->tex.Get() == surface) { ++s->refs; return; }
    }
    void release(void *surface)
    {
        std::lock_guard<std::mutex> lk(poolMutex);
        for (auto &s : pool) if (s->tex.Get() == surface) { if (s->refs > 0) --s->refs; return; }
    }

    // --- the MFT, event side ---
    void onEvent(MediaEventType type, HRESULT status)
    {
        if (type == METransformNeedInput) {
            Pending p{};
            bool have = false;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (!queue.empty()) { p = queue.front(); queue.pop_front(); have = true; }
                else ++needInput;
            }
            if (have) feed(p);
        } else if (type == METransformHaveOutput) {
            processOutput();
        } else if (type == METransformDrainComplete) {
            std::lock_guard<std::mutex> lk(mtx);
            drained = true;
            drainCv.notify_all();
        } else if (type == MEError) {
            errors.fetch_add(1, std::memory_order_relaxed);
            qWarning("MfVideoEncoder: MFT error event (%s)", hrText(status).c_str());
        }
    }

    void feed(const Pending &p)
    {
        if (p.key && codec) {
            VARIANT v = varU32(1);
            codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        }
        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), p.tex, 0, FALSE, buf.GetAddressOf());
        if (SUCCEEDED(hr)) hr = MFCreateSample(sample.GetAddressOf());
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf.Get());
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(to100ns(p.pts90k));
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(to100ns(p.dur90k > 0 ? p.dur90k : 3000));
        if (SUCCEEDED(hr)) {
            { std::lock_guard<std::mutex> lk(mtx); inFlight.push_back(p); }
            hr = mft->ProcessInput(inId, sample.Get(), 0);
            if (FAILED(hr)) {
                std::lock_guard<std::mutex> lk(mtx);
                if (!inFlight.empty() && inFlight.back().tex == p.tex) inFlight.pop_back();
            }
        }
        if (FAILED(hr)) {
            errors.fetch_add(1, std::memory_order_relaxed);
            if (errors.load() <= 3) qWarning("MfVideoEncoder: ProcessInput failed (%s)", hrText(hr).c_str());
            release(p.tex);
            return;
        }
        submitted.fetch_add(1, std::memory_order_relaxed);
    }

    void processOutput()
    {
        MFT_OUTPUT_DATA_BUFFER out{};
        out.dwStreamID = outId;
        ComPtr<IMFSample> ours;
        if (!providesSamples) {
            ComPtr<IMFMediaBuffer> mb;
            if (SUCCEEDED(MFCreateSample(ours.GetAddressOf()))
                && SUCCEEDED(MFCreateMemoryBuffer(outSize ? outSize : 2u * 1024u * 1024u, mb.GetAddressOf()))) {
                ours->AddBuffer(mb.Get());
                out.pSample = ours.Get();
            }
        }
        DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The encoder re-reports its output type once; accept it.
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(mft->GetOutputAvailableType(outId, 0, t.GetAddressOf()))) mft->SetOutputType(outId, t.Get(), 0);
            if (out.pSample && !ours) out.pSample->Release();
            return;
        }
        if (FAILED(hr)) {
            if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
                errors.fetch_add(1, std::memory_order_relaxed);
                if (errors.load() <= 3) qWarning("MfVideoEncoder: ProcessOutput failed (%s)", hrText(hr).c_str());
            }
            if (out.pSample && !ours) out.pSample->Release();
            return;
        }
        ComPtr<IMFSample> sample;
        if (ours) sample = ours; else sample.Attach(out.pSample);
        if (!sample) return;

        Pending done{};
        bool haveDone = false;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!inFlight.empty()) { done = inFlight.front(); inFlight.pop_front(); haveDone = true; }
        }
        emitSample(sample.Get(), haveDone ? &done : nullptr);
        if (haveDone) release(done.tex);
    }

    void emitSample(IMFSample *sample, const Pending *src)
    {
        UINT32 clean = 0;
        sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf()))) return;
        BYTE *p = nullptr; DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len)) || !p) return;

        // Walk the Annex-B units: remember SPS/PPS, note an IDR.
        bool idr = false, hasSps = false;
        std::vector<std::pair<size_t, size_t>> units;   // (offset of the unit after its start code, length)
        {
            size_t i = 0;
            auto findStart = [&](size_t from, size_t *scLen) -> size_t {
                for (size_t k = from; k + 3 <= len; ++k) {
                    if (p[k] == 0 && p[k + 1] == 0) {
                        if (p[k + 2] == 1) { *scLen = 3; return k; }
                        if (k + 4 <= len && p[k + 2] == 0 && p[k + 3] == 1) { *scLen = 4; return k; }
                    }
                }
                return len;
            };
            size_t sc = 0;
            size_t s = findStart(0, &sc);
            while (s < len) {
                const size_t unit = s + sc;
                size_t sc2 = 0;
                const size_t next = findStart(unit, &sc2);
                if (unit < len) units.emplace_back(unit, next - unit);
                s = next; sc = sc2;
                i = next;
            }
            (void)i;
        }
        for (const auto &[off, n] : units) {
            if (n == 0) continue;
            const int t = nalType(p + off);
            if (t == 7) { sps.assign(p + off, p + off + n); hasSps = true; }
            else if (t == 8) { pps.assign(p + off, p + off + n); }
            else if (t == 5) idr = true;
        }
        const bool key = idr || clean != 0;

        EncodedUnit au;
        au.keyframe = key;
        if (src) { au.pts = src->pts90k; au.dur = src->dur90k; }
        else {
            LONGLONG t = 0, d = 0;
            sample->GetSampleTime(&t); sample->GetSampleDuration(&d);
            au.pts = (t * 90000 + kHnsPerSec / 2) / kHnsPerSec;
            au.dur = (d * 90000 + kHnsPerSec / 2) / kHnsPerSec;
        }
        static const uint8_t kStart[4] = {0, 0, 0, 1};
        au.data.reserve(len + 128);
        // Parameter sets in band on every keyframe (TS and Louper want them).
        if (key && !hasSps && !sps.empty() && !pps.empty()) {
            au.data.insert(au.data.end(), kStart, kStart + 4);
            au.data.insert(au.data.end(), sps.begin(), sps.end());
            au.data.insert(au.data.end(), kStart, kStart + 4);
            au.data.insert(au.data.end(), pps.begin(), pps.end());
        }
        for (const auto &[off, n] : units) {
            if (n == 0) continue;
            au.data.insert(au.data.end(), kStart, kStart + 4);
            au.data.insert(au.data.end(), p + off, p + off + n);
        }
        buf->Unlock();
        if (au.data.empty()) return;
        encoded.fetch_add(1, std::memory_order_relaxed);
        if (key) keyframes.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(au.data.size(), std::memory_order_relaxed);
        if (userCb) userCb(std::move(au));
    }

    void armEvents()
    {
        if (gen && cb) gen->BeginGetEvent(cb, nullptr);
    }
};

STDMETHODIMP MfEventCallback::Invoke(IMFAsyncResult *result)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_impl || !m_impl->gen) return S_OK;
    ComPtr<IMFMediaEvent> ev;
    if (FAILED(m_impl->gen->EndGetEvent(result, ev.GetAddressOf())) || !ev) return S_OK;
    MediaEventType type = MEUnknown;
    HRESULT status = S_OK;
    ev->GetType(&type);
    ev->GetStatus(&status);
    m_impl->onEvent(type, status);
    if (m_impl->streaming) m_impl->armEvents();
    return S_OK;
}

MfVideoEncoder::MfVideoEncoder() : m_impl(std::make_unique<Impl>())
{
    const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    m_impl->mfStarted = SUCCEEDED(hr);
    if (!m_impl->mfStarted) qWarning("MfVideoEncoder: MFStartup failed (%s)", hrText(hr).c_str());
}

MfVideoEncoder::~MfVideoEncoder()
{
    close();
    releaseSurfaces();
    if (m_impl->mfStarted) MFShutdown();
}

bool MfVideoEncoder::isOpen() const { return m_impl->open.load(std::memory_order_acquire); }
const char *MfVideoEncoder::name() const { return m_impl->name.c_str(); }

bool MfVideoEncoder::prepareSurfaces(uint32_t width, uint32_t height, std::string *error)
{
    Impl &I = *m_impl;
    if (I.poolW == width && I.poolH == height) return true;
    releaseSurfaces();
    if (!I.device()) {
        if (error) *error = "no D3D11 device for the stream surfaces";
        return false;
    }
    std::lock_guard<std::mutex> lk(I.poolMutex);
    I.poolW = width; I.poolH = height;
    return true;
}

void MfVideoEncoder::releaseSurfaces()
{
    Impl &I = *m_impl;
    std::lock_guard<std::mutex> lk(I.poolMutex);
    I.pool.clear();
    I.poolW = I.poolH = 0;
}

void *MfVideoEncoder::acquireSurface()           { return m_impl->acquire(); }
void  MfVideoEncoder::retainSurface(void *s)     { if (s) m_impl->retain(s); }
void  MfVideoEncoder::releaseSurface(void *s)    { if (s) m_impl->release(s); }

bool MfVideoEncoder::open(const EncoderSpec &spec, Callback cb, std::string *error)
{
    close();
    Impl &I = *m_impl;
    I.spec = spec;
    I.userCb = std::move(cb);
    auto fail = [&](const std::string &msg) {
        if (error) *error = msg;
        qWarning("MfVideoEncoder: %s", msg.c_str());
        close();
        return false;
    };
    if (!I.mfStarted) return fail("Media Foundation is not available");
    ID3D11Device *dev = I.device();
    if (!dev) return fail("no D3D11 device");
    if (I.poolW != spec.width || I.poolH != spec.height) {
        std::string perr;
        if (!prepareSurfaces(spec.width, spec.height, &perr)) return fail(perr);
    }

    // The vendor's hardware H.264 encoder.
    MFT_REGISTER_TYPE_INFO outInfo{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate **acts = nullptr; UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           nullptr, &outInfo, &acts, &count);
    if (FAILED(hr) || count == 0 || !acts) {
        if (acts) CoTaskMemFree(acts);
        return fail("no hardware H.264 encoder registered with Media Foundation");
    }
    {
        LPWSTR fname = nullptr; UINT32 flen = 0;
        if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fname, &flen)) && fname) {
            char buf[160] = {};
            WideCharToMultiByte(CP_UTF8, 0, fname, -1, buf, sizeof buf - 1, nullptr, nullptr);
            I.name = std::string("Media Foundation H.264 (") + buf + ")";
            CoTaskMemFree(fname);
        }
        hr = acts[0]->ActivateObject(IID_PPV_ARGS(I.mft.GetAddressOf()));
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
    }
    if (FAILED(hr) || !I.mft) return fail("ActivateObject failed (" + hrText(hr) + ")");

    // Asynchronous MFT: unlock, hand over the device, low latency.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(I.mft->GetAttributes(attrs.GetAddressOf())) && attrs) {
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, 1);
        attrs->SetUINT32(MF_LOW_LATENCY, 1);
    }
    hr = MFCreateDXGIDeviceManager(&I.resetToken, I.mgr.GetAddressOf());
    if (SUCCEEDED(hr)) hr = I.mgr->ResetDevice(dev, I.resetToken);
    if (SUCCEEDED(hr)) hr = I.mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(I.mgr.Get()));
    if (FAILED(hr)) return fail("D3D manager rejected (" + hrText(hr) + ") — the device needs VIDEO_SUPPORT");
    {
        DWORD inIds[1] = {0}, outIds[1] = {0};
        if (SUCCEEDED(I.mft->GetStreamIDs(1, inIds, 1, outIds))) { I.inId = inIds[0]; I.outId = outIds[0]; }
    }

    // Session settings, before the types (a hardware MFT reads them then).
    I.mft.As(&I.codec);
    if (I.codec) {
        auto set = [&](const GUID &g, VARIANT v, const char *what) {
            const HRESULT r = I.codec->SetValue(&g, &v);
            if (FAILED(r)) qWarning("MfVideoEncoder: property %s rejected (%s)", what, hrText(r).c_str());
        };
        set(CODECAPI_AVLowLatencyMode,            varBool(true), "LowLatencyMode");
        set(CODECAPI_AVEncCommonRateControlMode,  varU32(eAVEncCommonRateControlMode_CBR), "RateControlMode");
        set(CODECAPI_AVEncCommonMeanBitRate,      varU32(spec.bitrateKbps * 1000u), "MeanBitRate");
        set(CODECAPI_AVEncMPVDefaultBPictureCount, varU32(0), "BPictureCount");
        set(CODECAPI_AVEncMPVGOPSize,             varU32(spec.gopFrames ? spec.gopFrames : 30u), "GOPSize");
        set(CODECAPI_AVEncCommonQualityVsSpeed,   varU32(33), "QualityVsSpeed");
    } else {
        qInfo("MfVideoEncoder: the encoder exposes no ICodecAPI; defaults in force");
    }

    const UINT64 frameSize = (static_cast<UINT64>(spec.width) << 32) | spec.height;
    const UINT64 frameRate = (static_cast<UINT64>(spec.fpsNum ? spec.fpsNum : 30) << 32) | (spec.fpsDen ? spec.fpsDen : 1);
    ComPtr<IMFMediaType> outType;
    hr = MFCreateMediaType(outType.GetAddressOf());
    if (SUCCEEDED(hr)) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT64(MF_MT_FRAME_SIZE, frameSize);
        outType->SetUINT64(MF_MT_FRAME_RATE, frameRate);
        outType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (1ull << 32) | 1ull);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT32(MF_MT_AVG_BITRATE, spec.bitrateKbps * 1000u);
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, spec.profile == H264Profile::Main ? eAVEncH264VProfile_Main : eAVEncH264VProfile_Base);
        outType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        outType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        outType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        outType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        hr = I.mft->SetOutputType(I.outId, outType.Get(), 0);
    }
    if (FAILED(hr)) return fail("SetOutputType(H.264) failed (" + hrText(hr) + ")");
    ComPtr<IMFMediaType> inType;
    hr = MFCreateMediaType(inType.GetAddressOf());
    if (SUCCEEDED(hr)) {
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inType->SetUINT64(MF_MT_FRAME_SIZE, frameSize);
        inType->SetUINT64(MF_MT_FRAME_RATE, frameRate);
        inType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (1ull << 32) | 1ull);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        inType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        inType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        inType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        inType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        hr = I.mft->SetInputType(I.inId, inType.Get(), 0);
    }
    if (FAILED(hr)) return fail("SetInputType(NV12) failed (" + hrText(hr) + ")");
    {
        MFT_OUTPUT_STREAM_INFO info{};
        if (SUCCEEDED(I.mft->GetOutputStreamInfo(I.outId, &info))) {
            I.providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            I.outSize = info.cbSize;
        }
    }
    hr = I.mft.As(&I.gen);
    if (FAILED(hr) || !I.gen) return fail("the encoder is not an event generator (" + hrText(hr) + ")");

    I.submitted = 0; I.encoded = 0; I.keyframes = 0; I.bytes = 0; I.errors = 0;
    {
        std::lock_guard<std::mutex> lk(I.mtx);
        I.queue.clear(); I.inFlight.clear(); I.needInput = 0; I.drained = false; I.streaming = true;
        I.sps.clear(); I.pps.clear();
    }
    I.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    I.cb = new MfEventCallback(&I);
    I.armEvents();
    I.open.store(true, std::memory_order_release);
    const double fps = spec.fpsDen ? double(spec.fpsNum) / double(spec.fpsDen) : 30.0;
    qInfo("MfVideoEncoder: %ux%u @ %.3f fps, %u kbps, %s, GOP %u — %s%s", spec.width, spec.height, fps,
          spec.bitrateKbps, spec.profile == H264Profile::Main ? "Main" : "Baseline", spec.gopFrames,
          I.name.c_str(), I.providesSamples ? "" : " (allocating output samples)");
    return true;
}

void MfVideoEncoder::close()
{
    Impl &I = *m_impl;
    if (!I.mft) return;
    I.open.store(false, std::memory_order_release);
    // Drop what never reached the MFT, then drain what did.
    std::deque<Impl::Pending> dropped;
    {
        std::lock_guard<std::mutex> lk(I.mtx);
        dropped.swap(I.queue);
    }
    for (const auto &p : dropped) I.release(p.tex);
    bool anyInFlight = false;
    { std::lock_guard<std::mutex> lk(I.mtx); anyInFlight = !I.inFlight.empty(); }
    if (anyInFlight) {
        I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        I.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        std::unique_lock<std::mutex> lk(I.mtx);
        I.drainCv.wait_for(lk, std::chrono::milliseconds(1500), [&] { return I.drained || I.inFlight.empty(); });
    }
    {
        std::lock_guard<std::mutex> lk(I.mtx);
        I.streaming = false;
        for (const auto &p : I.inFlight) I.release(p.tex);
        I.inFlight.clear();
    }
    I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    I.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (I.cb) { I.cb->detach(); I.cb->Release(); I.cb = nullptr; }
    I.codec.Reset();
    I.gen.Reset();
    I.mft.Reset();
    I.mgr.Reset();
    I.userCb = nullptr;
    qInfo("MfVideoEncoder: closed (%llu submitted, %llu encoded, %llu errors)",
          static_cast<unsigned long long>(I.submitted.load()), static_cast<unsigned long long>(I.encoded.load()),
          static_cast<unsigned long long>(I.errors.load()));
}

bool MfVideoEncoder::encode(void *surface, int64_t pts90k, int64_t dur90k, bool forceKeyframe)
{
    Impl &I = *m_impl;
    if (!I.open.load(std::memory_order_acquire) || !surface) return false;
    auto *tex = static_cast<ID3D11Texture2D *>(surface);
    I.retain(tex);   // ours until the MFT has produced this frame
    Impl::Pending p{tex, pts90k, dur90k, forceKeyframe};
    bool feedNow = false;
    {
        std::lock_guard<std::mutex> lk(I.mtx);
        if (I.needInput > 0) { --I.needInput; feedNow = true; }
        else {
            // Bounded: a stalled MFT must not pile up surfaces.
            if (I.queue.size() >= 4) {
                const Impl::Pending old = I.queue.front();
                I.queue.pop_front();
                I.release(old.tex);
                I.errors.fetch_add(1, std::memory_order_relaxed);
            }
            I.queue.push_back(p);
        }
    }
    if (feedNow) I.feed(p);
    return true;
}

VideoEncoder::Stats MfVideoEncoder::stats() const
{
    const Impl &I = *m_impl;
    return { I.submitted.load(), I.encoded.load(), I.keyframes.load(), I.bytes.load(), I.errors.load() };
}

std::unique_ptr<VideoEncoder> makePlatformVideoEncoder() { return std::make_unique<MfVideoEncoder>(); }

} // namespace qcv::output
