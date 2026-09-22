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
}

namespace qcv::dual {

DualLiveSource::DualLiveSource() = default;

DualLiveSource::~DualLiveSource()
{
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
    default:
        // Anything else (D3D11-decoded live on Windows) would need its own
        // DualFrame kind; the side holds its previous frame until then.
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

} // namespace qcv::dual
