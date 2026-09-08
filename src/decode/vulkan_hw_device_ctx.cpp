#include "vulkan_hw_device_ctx.h"

#if defined(Q_OS_WIN)

#include <QtLogging>

#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include <vulkan/vulkan.h>

#include "vulkan/vulkan_device_manager.h"

namespace qcv {

// Lifted from video_decoder.cpp (was anonymous-namespace, file-scope).
// The shape — qf[] entries, get_proc_addr explicit, device_features
// zero-init — was hardware-verified on Intel Arc 140T.
AVBufferRef *createSharedVulkanHwDeviceCtx()
{
    auto &dm = qcv::VulkanDeviceManager::instance();
    if (!dm.isInitialized()) {
        qWarning("createSharedVulkanHwDeviceCtx: VulkanDeviceManager not "
                 "initialized — main.cpp's startup init must run first");
        return nullptr;
    }
    // Phase I.B — refuse to wrap a known-poisoned device. The factory
    // is the choke point every decoder open() goes through; gating
    // here means any callsite (single-flow / dual-flow / scrub) falls
    // back to CPU cleanly without each having to check the flag.
    if (dm.isDeviceLost()) {
        qWarning("createSharedVulkanHwDeviceCtx: VkDevice is marked lost — "
                 "refusing to wrap a poisoned device; CPU fallback");
        return nullptr;
    }

    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!ref) {
        qWarning("createSharedVulkanHwDeviceCtx: av_hwdevice_ctx_alloc returned null");
        return nullptr;
    }

    auto *hwctx = reinterpret_cast<AVHWDeviceContext *>(ref->data);
    auto *vk    = reinterpret_cast<AVVulkanDeviceContext *>(hwctx->hwctx);

    // Explicit get_proc_addr — when supplying our own instance,
    // nullptr means "dynamically load libvulkan", which crashes on
    // Windows before any log fires. Routing through the loader's
    // vkGetInstanceProcAddr matches how we created the instance.
    vk->get_proc_addr = vkGetInstanceProcAddr;

    vk->inst     = dm.vkInstance();
    vk->phys_dev = dm.physicalDevice();
    vk->act_dev  = dm.device();

    vk->device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vk->device_features.pNext = nullptr;

    // Phase I.G — tell FFmpeg the queues were created internally
    // synchronized: (a) queue_flags so its vkGetDeviceQueue2 matches the
    // creation flags (mismatch = no queue), (b) the feature struct in the
    // device_features chain, which is what ff_vk_exec_pool_init() looks
    // for before it uses the flag and skips its own per-queue mutexes.
    // Static storage: FFmpeg keeps the pNext pointer for the context's
    // lifetime. lock_queue/unlock_queue below stay wired (harmless, and
    // still required by lavu 61); this is what replaces them at lavu 62.
#if defined(VK_KHR_internally_synchronized_queues) && LIBAVUTIL_VERSION_MAJOR >= 61
    static VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR s_isqFeatures{};
    if (dm.internallySyncedQueues()) {
        s_isqFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INTERNALLY_SYNCHRONIZED_QUEUES_FEATURES_KHR;
        s_isqFeatures.pNext = nullptr;
        s_isqFeatures.internallySynchronizedQueues = VK_TRUE;
        vk->device_features.pNext = &s_isqFeatures;
        vk->queue_flags = VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
    }
#endif

    vk->enabled_inst_extensions    = nullptr;
    vk->nb_enabled_inst_extensions = 0;

    // Hand over our enabled device-extension list verbatim. The
    // backing storage is a function-local static so the const char*
    // array lives as long as the AVBufferRef.
    static std::vector<const char *> s_devExtPtrs;
    s_devExtPtrs.clear();
    const auto &exts = dm.enabledDeviceExtensions();
    s_devExtPtrs.reserve(exts.size());
    for (const auto &e : exts) {
        s_devExtPtrs.push_back(e.c_str());
    }
    vk->enabled_dev_extensions    = s_devExtPtrs.data();
    vk->nb_enabled_dev_extensions = static_cast<int>(s_devExtPtrs.size());

    // Queue-family table (modern FFmpeg's qf[] layout). Dedup when
    // graphics/compute/transfer overlap; OR codec ops into video
    // decode entry.
    int nq = 0;
    auto pushQf = [&](uint32_t idx, VkQueueFlagBits flags, uint32_t videoCaps) {
        if (idx == UINT32_MAX) return;
        for (int j = 0; j < nq; ++j) {
            if (vk->qf[j].idx == static_cast<int>(idx)) {
                vk->qf[j].flags = static_cast<VkQueueFlagBits>(
                    static_cast<unsigned>(vk->qf[j].flags) | static_cast<unsigned>(flags));
                if (videoCaps) {
                    vk->qf[j].video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(
                        static_cast<unsigned>(vk->qf[j].video_caps) | videoCaps);
                }
                return;
            }
        }
        vk->qf[nq].idx        = static_cast<int>(idx);
        vk->qf[nq].num        = 1;
        vk->qf[nq].flags      = flags;
        vk->qf[nq].video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(videoCaps);
        ++nq;
    };
    pushQf(dm.graphicsFamily(),    VK_QUEUE_GRAPHICS_BIT,             0);
    pushQf(dm.computeFamily(),     VK_QUEUE_COMPUTE_BIT,              0);
    pushQf(dm.transferFamily(),    VK_QUEUE_TRANSFER_BIT,             0);
    pushQf(dm.videoDecodeFamily(), VK_QUEUE_VIDEO_DECODE_BIT_KHR,
           dm.videoDecodeCodecOps());
    vk->nb_qf = nq;

    // Phase I.D (2026-09-01) — wire FFmpeg per-queue lock callbacks
    // to the app-wide queue mutex. Left NULL, lavu installs its OWN
    // internal mutex, which serializes FFmpeg against FFmpeg only —
    // the renderer compositor dispatch and GUI-thread waitForGpu()
    // still raced FFmpeg vkQueueSubmit (playlist-boundary nvoglv64
    // crash on mixed-resolution ProRes playlists). Deprecated fields,
    // but honored while FF_API_VULKAN_SYNC_QUEUES holds (libavutil 60).
#if FF_API_VULKAN_SYNC_QUEUES
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    vk->lock_queue = [](AVHWDeviceContext *, uint32_t, uint32_t) {
        qcv::VulkanDeviceManager::instance().queueMutex().lock();
    };
    vk->unlock_queue = [](AVHWDeviceContext *, uint32_t, uint32_t) {
        qcv::VulkanDeviceManager::instance().queueMutex().unlock();
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif // FF_API_VULKAN_SYNC_QUEUES

    const int initErr = av_hwdevice_ctx_init(ref);
    if (initErr < 0) {
        qWarning("createSharedVulkanHwDeviceCtx: av_hwdevice_ctx_init failed (%d)", initErr);
        av_buffer_unref(&ref);
        return nullptr;
    }
    qInfo("createSharedVulkanHwDeviceCtx: shared Vulkan device handed to FFmpeg "
          "(%d queue families, %d device extensions)",
          nq, static_cast<int>(exts.size()));
    return ref;
}

// ---------------------------------------------------------------------
// Phase I.E — cached, app-owned Vulkan frame pools (see header).
// ---------------------------------------------------------------------
namespace {

struct FramesCacheEntry {
    const void       *device   = nullptr;   // VkDevice identity (see acquire)
    int               width    = 0;
    int               height   = 0;
    int               swFormat = AV_PIX_FMT_NONE;
    VkFormat          planeFmt[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
    VkImageUsageFlags usage    = 0;
    AVBufferRef      *frames   = nullptr;   // our cache ref (initialised)
};

std::mutex                    g_framesCacheMutex;
std::vector<FramesCacheEntry> g_framesCache;

} // namespace

AVBufferRef *acquireSharedVulkanFramesCtx(AVCodecContext *avctx)
{
    if (!avctx || !avctx->hw_device_ctx) return nullptr;
    const auto *dev =
        reinterpret_cast<const AVHWDeviceContext *>(avctx->hw_device_ctx->data);
    if (!dev || dev->type != AV_HWDEVICE_TYPE_VULKAN) return nullptr;
    // Key on the VkDevice, not the AVHWDeviceContext: every context
    // createSharedVulkanHwDeviceCtx() hands out wraps the SAME
    // VulkanDeviceManager device (same queues, same lock callbacks),
    // and a decoder that visits a D3D11VA clip in between comes back
    // with a fresh AVHWDeviceContext pointer. The pool keeps its own
    // device_ref alive, so the context it was built on outlives it.
    const void *deviceKey =
        reinterpret_cast<const AVVulkanDeviceContext *>(dev->hwctx)->act_dev;

    // Let the hwaccel describe the pool it needs (dims incl. coded
    // alignment, sw_format, per-plane VkFormats, usage, create_pnext).
    AVBufferRef *fresh = nullptr;
    int err = avcodec_get_hw_frames_parameters(avctx, avctx->hw_device_ctx,
                                               AV_PIX_FMT_VULKAN, &fresh);
    if (err < 0 || !fresh) {
        qWarning("acquireSharedVulkanFramesCtx: avcodec_get_hw_frames_parameters "
                 "failed (%d) — FFmpeg will manage the pool", err);
        return nullptr;
    }
    auto *fc  = reinterpret_cast<AVHWFramesContext *>(fresh->data);
    auto *vfc = reinterpret_cast<AVVulkanFramesContext *>(fc->hwctx);

    // Key = the hwaccel's REQUEST, captured before av_hwframe_ctx_init:
    // vulkan_frames_init widens `usage` to what the device supports
    // and fills the per-plane VkFormats, so post-init values never
    // equal the next request's pre-init values.
    FramesCacheEntry key;
    key.device   = deviceKey;
    key.width    = fc->width;
    key.height   = fc->height;
    key.swFormat = fc->sw_format;
    std::memcpy(key.planeFmt, vfc->format, sizeof(key.planeFmt));
    key.usage    = vfc->usage;

    std::lock_guard<std::mutex> lock(g_framesCacheMutex);
    for (const FramesCacheEntry &e : g_framesCache) {
        if (e.device == key.device && e.width == key.width
            && e.height == key.height && e.swFormat == key.swFormat
            && e.usage == key.usage
            && std::memcmp(e.planeFmt, key.planeFmt, sizeof(e.planeFmt)) == 0) {
            av_buffer_unref(&fresh);
            qInfo("acquireSharedVulkanFramesCtx: pool HIT %dx%d %s (%zu cached)",
                  e.width, e.height,
                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(e.swFormat)),
                  g_framesCache.size());
            return av_buffer_ref(e.frames);
        }
    }

    // Mirror ff_decode_get_hw_frames_ctx's headroom for fixed-size
    // pools (Vulkan pools are dynamic → initial_pool_size is 0 and
    // this is a no-op, kept for parity).
    if (fc->initial_pool_size) {
        const int extra = avctx->extra_hw_frames > 0 ? avctx->extra_hw_frames : 0;
        fc->initial_pool_size += 3 + extra;
    }
    if ((err = av_hwframe_ctx_init(fresh)) < 0) {
        qWarning("acquireSharedVulkanFramesCtx: av_hwframe_ctx_init failed "
                 "(%d) — FFmpeg will manage the pool", err);
        av_buffer_unref(&fresh);
        return nullptr;
    }

    key.frames = av_buffer_ref(fresh);
    g_framesCache.push_back(key);
    qInfo("acquireSharedVulkanFramesCtx: pool allocated %dx%d %s (%zu cached)",
          key.width, key.height,
          av_get_pix_fmt_name(static_cast<AVPixelFormat>(key.swFormat)),
          g_framesCache.size());
    return fresh;
}

void releaseSharedVulkanFramesCache()
{
    std::vector<AVBufferRef *> refs;
    {
        std::lock_guard<std::mutex> lock(g_framesCacheMutex);
        refs.reserve(g_framesCache.size());
        for (FramesCacheEntry &e : g_framesCache) refs.push_back(e.frames);
        g_framesCache.clear();
    }
    // Unref outside the lock — a last-ref drop runs FFmpeg's pool
    // teardown (semaphore waits + vkDestroyImage) and must not hold
    // our cache mutex while doing so.
    for (AVBufferRef *r : refs) av_buffer_unref(&r);
    if (!refs.empty())
        qInfo("releaseSharedVulkanFramesCache: dropped %zu cached pool(s)",
              refs.size());
}

} // namespace qcv

#endif // Q_OS_WIN
