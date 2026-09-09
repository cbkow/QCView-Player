#include "d3d11va_hw_device_ctx.h"

#if defined(Q_OS_WIN)

#include <QtLogging>

#include <cstring>
#include <mutex>
#include <vector>

// D3D11 headers FIRST: hwcontext_d3d11va.h includes <d3d11.h>, and doing
// that inside an extern "C" block gives its C++ operator overloads C
// linkage (C2733). Pre-including them makes the guard skip the re-include.
#include <d3d11.h>
#include <d3d10.h>   // ID3D10Multithread (the interface D3D11 still uses)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

namespace qcv {

namespace {

struct SharedDevice {
    ID3D11Device        *device  = nullptr;   // AddRef'd
    ID3D11DeviceContext *context = nullptr;   // AddRef'd
};

std::mutex   g_deviceMutex;
SharedDevice g_shared;

struct PoolEntry {
    const void   *device   = nullptr;   // ID3D11Device identity
    int           width    = 0;
    int           height   = 0;
    int           swFormat = AV_PIX_FMT_NONE;
    AVBufferRef  *frames   = nullptr;   // our cache ref (initialised)
};

std::mutex             g_poolMutex;
std::vector<PoolEntry> g_pools;

void releasePoolsLocked(std::vector<PoolEntry> &&pools)
{
    // Unref outside the pool mutex: a last-ref drop destroys the
    // texture array; D3D11 defers the actual GPU release until any
    // in-flight command that reads it has retired, so no fence dance.
    for (PoolEntry &e : pools) av_buffer_unref(&e.frames);
}

} // namespace

void setSharedD3D11Device(void *id3d11Device, void *id3d11ImmediateContext)
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    if (g_shared.device)  g_shared.device->Release();
    if (g_shared.context) g_shared.context->Release();
    g_shared.device  = static_cast<ID3D11Device *>(id3d11Device);
    g_shared.context = static_cast<ID3D11DeviceContext *>(id3d11ImmediateContext);
    if (g_shared.device)  g_shared.device->AddRef();
    if (g_shared.context) g_shared.context->AddRef();

    // FFmpeg's D3D11VA hwaccel calls into the immediate context from its
    // decode threads while the render thread uses it too. The D3D11
    // runtime serializes both once multithread protection is on.
    if (g_shared.context) {
        ID3D10Multithread *mt = nullptr;
        if (SUCCEEDED(g_shared.context->QueryInterface(__uuidof(ID3D10Multithread),
                                                       reinterpret_cast<void **>(&mt)))
            && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
            qInfo("d3d11va_hw_device_ctx: shared ID3D11Device registered "
                  "(multithread protection ON)");
        } else {
            qWarning("d3d11va_hw_device_ctx: ID3D10Multithread unavailable — "
                     "shared-device D3D11VA disabled");
            g_shared.device->Release();  g_shared.device  = nullptr;
            g_shared.context->Release(); g_shared.context = nullptr;
        }
    }
}

void clearSharedD3D11Device()
{
    releaseSharedD3D11VaFramesCache();
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    if (g_shared.device)  { g_shared.device->Release();  g_shared.device  = nullptr; }
    if (g_shared.context) { g_shared.context->Release(); g_shared.context = nullptr; }
}

bool hasSharedD3D11Device()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    return g_shared.device != nullptr;
}

bool isSharedD3D11VaDeviceCtx(const AVBufferRef *hwDeviceCtx)
{
    if (!hwDeviceCtx) return false;
    const auto *dev = reinterpret_cast<const AVHWDeviceContext *>(hwDeviceCtx->data);
    if (!dev || dev->type != AV_HWDEVICE_TYPE_D3D11VA) return false;
    const auto *dc = reinterpret_cast<const AVD3D11VADeviceContext *>(dev->hwctx);
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    return dc && g_shared.device && dc->device == g_shared.device;
}

AVBufferRef *createSharedD3D11VaHwDeviceCtx()
{
    ID3D11Device        *device  = nullptr;
    ID3D11DeviceContext *context = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        if (!g_shared.device || !g_shared.context) return nullptr;
        device  = g_shared.device;
        context = g_shared.context;
    }

    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) return nullptr;
    auto *hwctx = reinterpret_cast<AVHWDeviceContext *>(ref->data);
    auto *dc    = reinterpret_cast<AVD3D11VADeviceContext *>(hwctx->hwctx);

    // FFmpeg releases these in its uninit — hand it its own references.
    device->AddRef();
    context->AddRef();
    dc->device         = device;
    dc->device_context = context;
    // video_device / video_context: FFmpeg QIs them from the above.
    // lock / unlock: FFmpeg installs its own critical section when NULL.

    const int err = av_hwdevice_ctx_init(ref);
    if (err < 0) {
        qWarning("createSharedD3D11VaHwDeviceCtx: av_hwdevice_ctx_init failed (%d)", err);
        av_buffer_unref(&ref);
        return nullptr;
    }
    return ref;
}

AVBufferRef *acquireSharedD3D11VaFramesCtx(AVCodecContext *avctx)
{
    if (!avctx || !isSharedD3D11VaDeviceCtx(avctx->hw_device_ctx)) return nullptr;
    const auto *dev = reinterpret_cast<const AVHWDeviceContext *>(avctx->hw_device_ctx->data);
    const auto *dc  = reinterpret_cast<const AVD3D11VADeviceContext *>(dev->hwctx);
    const void *deviceKey = dc->device;

    AVBufferRef *fresh = nullptr;
    int err = avcodec_get_hw_frames_parameters(avctx, avctx->hw_device_ctx,
                                               AV_PIX_FMT_D3D11, &fresh);
    if (err < 0 || !fresh) {
        qWarning("acquireSharedD3D11VaFramesCtx: avcodec_get_hw_frames_parameters "
                 "failed (%d) — FFmpeg will manage the pool", err);
        return nullptr;
    }
    auto *fc  = reinterpret_cast<AVHWFramesContext *>(fresh->data);
    auto *dfc = reinterpret_cast<AVD3D11VAFramesContext *>(fc->hwctx);

    // The hwaccel asks for D3D11_BIND_DECODER; the renderer samples the
    // slices, so add SHADER_RESOURCE (both on one array is standard).
    dfc->BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    PoolEntry key;
    key.device   = deviceKey;
    key.width    = fc->width;
    key.height   = fc->height;
    key.swFormat = fc->sw_format;

    std::lock_guard<std::mutex> lock(g_poolMutex);
    for (const PoolEntry &e : g_pools) {
        if (e.device == key.device && e.width == key.width
            && e.height == key.height && e.swFormat == key.swFormat) {
            av_buffer_unref(&fresh);
            qInfo("acquireSharedD3D11VaFramesCtx: pool HIT %dx%d %s (%zu cached)",
                  e.width, e.height,
                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(e.swFormat)),
                  g_pools.size());
            return av_buffer_ref(e.frames);
        }
    }

    // Same headroom ff_decode_get_hw_frames_ctx adds (a D3D11VA pool is
    // a fixed-size texture array, so the count matters here).
    if (fc->initial_pool_size) {
        const int extra = avctx->extra_hw_frames > 0 ? avctx->extra_hw_frames : 0;
        fc->initial_pool_size += 3 + extra;
    }
    if ((err = av_hwframe_ctx_init(fresh)) < 0) {
        qWarning("acquireSharedD3D11VaFramesCtx: av_hwframe_ctx_init failed (%d) — "
                 "FFmpeg will manage the pool", err);
        av_buffer_unref(&fresh);
        return nullptr;
    }

    key.frames = av_buffer_ref(fresh);
    g_pools.push_back(key);
    qInfo("acquireSharedD3D11VaFramesCtx: pool allocated %dx%d %s x%d (%zu cached)",
          key.width, key.height,
          av_get_pix_fmt_name(static_cast<AVPixelFormat>(key.swFormat)),
          fc->initial_pool_size, g_pools.size());
    return fresh;
}

void releaseSharedD3D11VaFramesCache()
{
    std::vector<PoolEntry> pools;
    {
        std::lock_guard<std::mutex> lock(g_poolMutex);
        pools.swap(g_pools);
    }
    if (!pools.empty())
        qInfo("releaseSharedD3D11VaFramesCache: dropping %zu cached pool(s)",
              pools.size());
    releasePoolsLocked(std::move(pools));
}

bool d3d11FrameIsZeroCopyConsumable(const AVFrame *frame)
{
    if (!frame || frame->format != AV_PIX_FMT_D3D11 || !frame->hw_frames_ctx) return false;
    const auto *fc = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
    if (!fc || !isSharedD3D11VaDeviceCtx(fc->device_ref)) return false;
    return fc->sw_format == AV_PIX_FMT_NV12 || fc->sw_format == AV_PIX_FMT_P010;
}

} // namespace qcv

#endif // Q_OS_WIN
