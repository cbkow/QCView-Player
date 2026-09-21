// HostBridgeSource — After Effects / Premiere frames from the QCBridgeAE
// Transmit device, over same-machine shared memory.
//
// QCBridgeAE's device converts each frame the host pushes to top-down
// RGBA16F, in the host's working colour space, never clamped (inf/NaN are
// carried and flagged), and publishes it latest-wins into a shared ring:
// /qcbae-ae for After Effects, /qcbae-premiere for Premiere. The protocol is
// vendored in decode/qcbae/ (see the header there). This class is the
// consumer: a worker thread watches the ring and publishes each new frame
// into the main VideoDecoder's slot, like LiveStreamDecoder.
//
// URLs are qcbae://ae, qcbae://premiere and (development) qcbae://probe for
// QCBridgeAE's synthetic producer. The URL form keeps every "://"-keyed live
// behaviour in QCView (routing, persistence, dual guards) working.
//
// What the ring tells us, and what the UI shows:
//   no ring yet / producer never seen  -> Connecting ("Waiting for …")
//   producer process gone              -> Reconnecting (AE quits without
//                                         unloading the device, so the ring
//                                         outlives it; liveness is the pid)
//   ring retired (replaced)            -> re-open by name, immediately
//   host paused the feed               -> Paused, last frame held; for a
//                                         focus-loss pause, the preference
//                                         that stops it
//   frames                             -> Live
//
// Frames are copied out of the ring, not viewed in place: the ring grants
// one reader claim, and the renderer uploads a published frame later on its
// own thread, so a view would be overwritten once this reader moved on. The
// copy goes into a small pool of reused QImages (no 66 MB allocation per 4K
// frame). Zero-copy is a later step (QCBridgeAE PLAN-A3).

#pragma once

#include "decode/live_source.h"
#include "decode/qcbae/host_bridge_url.h"

#include <QImage>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace qcv {

class HostBridgeSource : public LiveSource
{
    Q_OBJECT

public:
    explicit HostBridgeSource(QObject *parent = nullptr);
    ~HostBridgeSource() override;

    void setSink(VideoDecoder *sink) override { m_sink = sink; }
    void setFrameCallback(std::function<void()> cb) override;
    bool open(const QString &url) override;
    void close() override;

    Status  status() const override {
        return static_cast<Status>(m_status.load(std::memory_order_acquire));
    }
    QString statusDetail() const override;
    QString url() const override { return m_url; }
    int     width() const override { return m_width.load(std::memory_order_acquire); }
    int     height() const override { return m_height.load(std::memory_order_acquire); }
    QString codecName() const override;
    QString pixelFormatName() const override { return QStringLiteral("RGBA16F"); }
    int     reconnectCount() const override { return m_reconnects.load(std::memory_order_acquire); }
    bool    nonFinite() const override { return m_nonFinite.load(std::memory_order_acquire); }
    bool    sharedMemory() const override { return true; }

    double statFramesReceived() const override {
        return double(m_framesReceived.load(std::memory_order_acquire));
    }
    double statBytesReceived() const override {
        return double(m_bytesReceived.load(std::memory_order_acquire));
    }
    int statLiveSeconds() const override;

private:
    void workerLoop();
    void setStatus(Status s, const QString &detail);
    void setGeometry(int w, int h);
    void setNonFinite(bool v);
    bool interruptibleSleep(int ms);   // false when close() interrupted it
    QImage *takePoolImage(int w, int h);

    VideoDecoder         *m_sink = nullptr;
    QString               m_url;
    QString               m_ringName;
    QString               m_hostLabel;

    std::thread           m_thread;
    std::atomic<bool>     m_stopRequested{false};
    std::mutex            m_sleepMutex;
    std::condition_variable m_sleepCv;

    std::atomic<int>      m_status{Idle};
    std::atomic<int>      m_width{0};
    std::atomic<int>      m_height{0};
    std::atomic<int>      m_reconnects{0};
    std::atomic<bool>     m_nonFinite{false};
    std::atomic<qint64>   m_framesReceived{0};
    std::atomic<qint64>   m_bytesReceived{0};
    std::atomic<qint64>   m_liveSinceMs{0};   // steady_clock ms; 0 = not live

    mutable std::mutex    m_detailMutex;
    QString               m_detail;

    std::mutex            m_frameCbMutex;
    std::function<void()> m_frameCb;

    // Reused frame buffers, worker thread only. An image is reusable once
    // the renderer has let go of every copy of it (QImage::isDetached()).
    std::vector<QImage>   m_pool;
};

} // namespace qcv
