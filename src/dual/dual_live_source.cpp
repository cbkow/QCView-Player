#include "dual_live_source.h"

#include "decode/host_bridge_source.h"
#include "decode/live_stream_decoder.h"
#include "decode/qcbae/host_bridge_url.h"

#include <QtLogging>

#if defined(Q_OS_MACOS)
// Retain / release shims for CVPixelBufferRef, so this stays a plain .cpp
// (dual_cv_helpers.mm; same contract DualVideoDecoder uses).
extern "C" void dualCvPixelBufferRetain(void *cvPix);
extern "C" void dualCvPixelBufferRelease(void *cvPix);
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#if defined(Q_OS_WIN)
#include "decode/rgb_range.h"
#include "decode/sws_rgba_image.h"
#include "decode/sws_threaded.h"
#endif

namespace qcv::dual {

DualLiveSource::DualLiveSource() = default;

DualLiveSource::~DualLiveSource()
{
#if defined(Q_OS_WIN)
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_swFrame) av_frame_free(&m_swFrame);
#endif
    close();
}

bool DualLiveSource::open(const QString &path)
{
    close();
    if (path.isEmpty()) return false;

    // Same choice WindowManager::startLiveStream makes for single view.
    const bool hostBridge = qcv::hostbridge::isUrl(path);
#ifdef QCV_HAS_HOST_BRIDGE
    if (hostBridge) m_live = std::make_unique<qcv::HostBridgeSource>();
    else            m_live = std::make_unique<qcv::LiveStreamDecoder>();
#else
    if (hostBridge) {
        qWarning("DualLiveSource: %s needs the QCBridge host bridge, which "
                 "this build does not have", qPrintable(path));
        return false;
    }
    m_live = std::make_unique<qcv::LiveStreamDecoder>();
#endif

    m_url = path;
    m_live->setSink(this);
    if (!m_live->open(path)) {
        qWarning("DualLiveSource: could not open %s", qPrintable(path));
        m_live.reset();
        m_url.clear();
        return false;
    }
    return true;
}

void DualLiveSource::close()
{
    if (m_live) {
        // Joins the receive worker: nothing publishes after this returns.
        m_live->close();
        m_live.reset();
    }
    {
        std::lock_guard<std::mutex> lk(m_frameMutex);
        m_latest.reset();
    }
    {
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        m_onFrameAvailable = nullptr;
    }
    m_url.clear();
}

bool DualLiveSource::isOpen() const { return m_live != nullptr; }

void DualLiveSource::setDecodeTarget(int) {}   // nothing to decode towards
void DualLiveSource::seekTo(int) {}            // nothing to seek in

std::shared_ptr<DualFrame> DualLiveSource::getBufferedFrame(int) const
{
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_latest;
}

std::shared_ptr<DualFrame> DualLiveSource::getClosestFrame(int) const
{
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_latest;   // the master frame means nothing here
}

bool DualLiveSource::hasFrame(int) const
{
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_latest != nullptr;
}

int DualLiveSource::bufferedAhead() const  { return 0; }
int DualLiveSource::bufferedBehind() const { return 0; }

int DualLiveSource::bufferCount() const
{
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_latest ? 1 : 0;
}

void DualLiveSource::getBufferedRange(int &startFrame, int &endFrame) const
{
    startFrame = -1;
    endFrame   = -1;
}

int DualLiveSource::width() const  { return m_live ? m_live->width()  : 0; }
int DualLiveSource::height() const { return m_live ? m_live->height() : 0; }

QString DualLiveSource::hwAccelName() const
{
    return m_live ? m_live->codecName() : QString();
}

void DualLiveSource::setFrameAvailableCallback(FrameAvailableCallback cb)
{
    std::lock_guard<std::mutex> lk(m_callbackMutex);
    m_onFrameAvailable = std::move(cb);
}

// Receiver thread. Takes the handle's frame into the slot, converting each
// kind the way DualVideoDecoder does, so the compositor sees frames that are
// indistinguishable from a decoded side's.
void DualLiveSource::publishExternalFrame(qcv::FrameHandle handle, int64_t)
{
    auto out = std::make_shared<DualFrame>();
    out->frameNumber = -1;   // free-running: no timeline position
    out->width  = handle.width();
    out->height = handle.height();

    switch (handle.kind()) {
    case qcv::FrameHandle::Kind::Cpu: {
        const QImage &img = handle.cpuImage();
        if (img.isNull()) return;
        // Implicitly shared: this is a refcount bump, not a pixel copy.
        out->kind   = DualFrame::Kind::Cpu;
        out->rgba   = std::make_shared<QImage>(img);
        out->width  = img.width();
        out->height = img.height();
        break;
    }
#if defined(Q_OS_MACOS)
    case qcv::FrameHandle::Kind::Metal: {
        void *cvPix = handle.metalPixelBuffer();
        if (!cvPix) return;
        // The handle releases its own reference when it goes out of scope.
        dualCvPixelBufferRetain(cvPix);
        out->kind          = DualFrame::Kind::Metal;
        out->cvPixelBuffer = std::shared_ptr<void>(cvPix, dualCvPixelBufferRelease);
        break;
    }
#endif
#if defined(Q_OS_WIN)
    case qcv::FrameHandle::Kind::Vulkan: {
        AVFrame *cloned = av_frame_clone(handle.vulkanAvFrame());
        if (!cloned) return;
        out->kind    = DualFrame::Kind::Vulkan;
        out->avFrame = std::shared_ptr<void>(
            static_cast<void *>(cloned),
            [](void *p) { AVFrame *f = static_cast<AVFrame *>(p); av_frame_free(&f); });
        break;
    }
#endif
#if defined(Q_OS_WIN)
    case qcv::FrameHandle::Kind::D3D11: {
        auto cpu = publishD3D11(handle);
        if (!cpu) return;
        out = std::move(cpu);
        break;
    }
#endif
    default:
        // Anything else would need its own DualFrame kind; the side holds
        // its previous frame until then.
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_frameMutex);
        m_latest = std::move(out);   // latest wins; a slow consumer skips
    }

    FrameAvailableCallback cb;
    {
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        cb = m_onFrameAvailable;
    }
    if (cb) cb();   // wakes D3D11's render-on-demand loop; Metal free-runs
}

#if defined(Q_OS_WIN)
// The D3D11VA slice comes down to the CPU (av_hwframe_transfer_data into
// NV12/P010) and through swscale to RGBA8 or RGBA64, the same route and
// the same depth rule (rgb_range.h) as DualVideoDecoder's D3D11VA file
// side. One readback per frame; a D3D11 DualFrame kind would be the
// zero-copy version of this, and until it exists a moving picture beats a
// frozen one. Found on the Windows machine 2026-09-23 with a QCBridge
// stream on one side of dual.
std::shared_ptr<DualFrame> DualLiveSource::publishD3D11(const qcv::FrameHandle &handle)
{
    AVFrame *frame = handle.d3d11AvFrame();
    if (!frame) return nullptr;
    if (!m_swFrame) m_swFrame = av_frame_alloc();
    if (!m_swFrame) return nullptr;
    av_frame_unref(m_swFrame);
    if (int err = av_hwframe_transfer_data(m_swFrame, frame, 0); err < 0) {
        qWarning("DualLiveSource: av_hwframe_transfer_data failed (%d); frame dropped", err);
        return nullptr;
    }
    m_swFrame->colorspace      = frame->colorspace;
    m_swFrame->color_range     = frame->color_range;
    m_swFrame->color_primaries = frame->color_primaries;
    m_swFrame->color_trc       = frame->color_trc;
    const AVFrame *src = m_swFrame;

    const AVPixelFormat dstFmt = qcv::cpuPublishPixelFormat(src->format);
    if (!(m_sws && m_swsSrcW == src->width && m_swsSrcH == src->height
          && m_swsSrcFmt == src->format && m_swsDstFmt == dstFmt)) {
        if (m_sws) sws_freeContext(m_sws);
        m_sws = qcv::swsCreateThreaded();
        if (!m_sws) return nullptr;
        m_swsSrcW = src->width; m_swsSrcH = src->height;
        m_swsSrcFmt = src->format; m_swsDstFmt = dstFmt;
    }
    const bool sixteen = (dstFmt == AV_PIX_FMT_RGBA64LE);
    if (!m_loggedD3D11) {
        m_loggedD3D11 = true;
        qInfo("DualLiveSource: D3D11VA live frames brought to the CPU for dual (%dx%d, %s)",
              src->width, src->height, sixteen ? "RGBA64" : "RGBA8");
    }
    auto out = std::make_shared<DualFrame>();
    out->frameNumber = -1;
    out->width  = src->width;
    out->height = src->height;
    out->kind   = DualFrame::Kind::Cpu;
    out->rgba   = std::make_shared<QImage>(
        qcv::swsAllocImage(src->width, src->height,
                           sixteen ? QImage::Format_RGBA64 : QImage::Format_RGBA8888));
    if (qcv::swsConvertToBuffer(m_sws, src, dstFmt, out->rgba->bits(),
                                static_cast<int>(out->rgba->bytesPerLine()), 0) < 0) {
        qWarning("DualLiveSource: sws_scale_frame failed; frame dropped");
        return nullptr;
    }
    if (qcv::rgbFrameNeedsLegalExpansion(src, 0)) {
        if (sixteen) {
            qcv::expandRgba16LegalToFull(reinterpret_cast<uint16_t *>(out->rgba->bits()),
                                         src->width, src->height,
                                         static_cast<int>(out->rgba->bytesPerLine()));
        } else {
            qcv::expandRgba8LegalToFull(out->rgba->bits(), src->width, src->height,
                                        static_cast<int>(out->rgba->bytesPerLine()));
        }
    }
    return out;
}
#endif

} // namespace qcv::dual
