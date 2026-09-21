#include "host_bridge_source.h"
#include "video_decoder.h"
#include "decode/frame_handle.h"
#include "decode/qcbae/shared_ring.h"

#include <QMetaObject>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <signal.h>

namespace qcv {

namespace {

constexpr int kWaitPollMs  = 250;   // ring absent or producer gone
constexpr int kFramePollMs = 2;     // ring healthy: how often to look for a frame

qint64 steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// AE quits without unloading the Transmit device, so its ring outlives it,
// readable, with a frozen host state. The producer's pid is the only truth.
bool producerAlive(const qcbae::SharedRing &ring)
{
    const auto pid = static_cast<pid_t>(ring.header()->producer_pid);
    return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

} // namespace

HostBridgeSource::HostBridgeSource(QObject *parent)
    : LiveSource(parent)
{
}

HostBridgeSource::~HostBridgeSource()
{
    close();
}

void HostBridgeSource::setFrameCallback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lk(m_frameCbMutex);
    m_frameCb = std::move(cb);
}

bool HostBridgeSource::open(const QString &url)
{
    if (m_thread.joinable()) close();
    if (!m_sink) {
        qWarning("HostBridgeSource: no sink VideoDecoder set — refusing open");
        return false;
    }
    m_ringName = hostbridge::ringName(url);
    if (m_ringName.isEmpty()) {
        qWarning("HostBridgeSource: '%s' is not a qcbae:// source", qPrintable(url));
        return false;
    }
    m_url = url;
    m_hostLabel = hostbridge::label(url);
    m_stopRequested.store(false, std::memory_order_release);
    m_reconnects.store(0, std::memory_order_release);
    m_framesReceived.store(0, std::memory_order_release);
    m_bytesReceived.store(0, std::memory_order_release);
    setStatus(Connecting, QStringLiteral("Waiting for %1").arg(m_hostLabel));
    m_thread = std::thread([this] { workerLoop(); });
    return true;
}

void HostBridgeSource::close()
{
    if (!m_thread.joinable()) return;
    m_stopRequested.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(m_sleepMutex); }
    m_sleepCv.notify_all();
    m_thread.join();
    m_pool.clear();
    setStatus(Idle, {});
    qInfo("HostBridgeSource: closed ('%s')", qPrintable(m_url));
}

QString HostBridgeSource::statusDetail() const
{
    std::lock_guard<std::mutex> lk(m_detailMutex);
    return m_detail;
}

QString HostBridgeSource::codecName() const
{
    return QStringLiteral("Transmit · %1").arg(m_hostLabel);
}

int HostBridgeSource::statLiveSeconds() const
{
    if (status() != Live) return 0;
    const qint64 since = m_liveSinceMs.load(std::memory_order_acquire);
    return since > 0 ? int((steadyMs() - since) / 1000) : 0;
}

bool HostBridgeSource::interruptibleSleep(int ms)
{
    std::unique_lock<std::mutex> lk(m_sleepMutex);
    return !m_sleepCv.wait_for(lk, std::chrono::milliseconds(ms), [this] {
        return m_stopRequested.load(std::memory_order_acquire);
    });
}

QImage *HostBridgeSource::takePoolImage(int w, int h)
{
    for (QImage &img : m_pool) {
        if (img.width() == w && img.height() == h && img.isDetached()) return &img;
    }
    // Three suffice in steady state (one with the renderer, one in the slot,
    // one being filled); a burst of size changes may briefly add more.
    if (m_pool.size() >= 6) m_pool.erase(m_pool.begin());
    m_pool.emplace_back(w, h, QImage::Format_RGBA16FPx4);
    return &m_pool.back();
}

void HostBridgeSource::workerLoop()
{
    const std::string ringName = m_ringName.toStdString();
    const qint64 sessionStartMs = steadyMs();
    qcbae::SharedRing ring;
    uint64_t lastSeen = 0;
    bool everLive = false;
    bool loggedFirst = false;
    uint32_t loggedPid = 0;   // log a ring (re)open once per producer, not per poll

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        if (!ring.valid()) {
            if (!ring.open(ringName)) {
                const QString err = QString::fromStdString(ring.error());
                // A version mismatch is not "waiting": say what is wrong.
                const QString detail = err.contains(QLatin1String("version"))
                    ? QStringLiteral("Incompatible QCBridgeAE plugin (%1)").arg(err)
                    : QStringLiteral("Waiting for %1").arg(m_hostLabel);
                setStatus(everLive ? Reconnecting : Connecting, detail);
                if (!interruptibleSleep(kWaitPollMs)) break;
                continue;
            }
            lastSeen = 0;
            // A dead producer's ring stays readable (AE never unlinks it), so
            // while nothing new is publishing this re-opens it every poll.
            if (ring.header()->producer_pid != loggedPid) {
                loggedPid = ring.header()->producer_pid;
                qInfo("HostBridgeSource: ring %s open (producer pid %u)",
                      ringName.c_str(), loggedPid);
            }
        }

        if (!producerAlive(ring)) {
            ring = qcbae::SharedRing();
            if (everLive) m_reconnects.fetch_add(1, std::memory_order_acq_rel);
            setStatus(everLive ? Reconnecting : Connecting,
                      QStringLiteral("%1 is not running").arg(m_hostLabel));
            if (!interruptibleSleep(kWaitPollMs)) break;
            continue;
        }

        const qcbae::HostState hs = ring.host_state();
        if (hs == qcbae::HostState::Retired) {   // producer moved to a new mapping
            ring = qcbae::SharedRing();
            continue;
        }

        qcbae::FrameDesc d {};
        const void *pixels = nullptr;
        if (!ring.acquire_latest(&lastSeen, &d, &pixels)) {
            if (hs == qcbae::HostState::PausedFocus) {
                setStatus(Paused, QStringLiteral(
                    "%1 stopped sending when it lost focus — untick “Disable video "
                    "output when in the background” in its preferences").arg(m_hostLabel));
            } else if (hs == qcbae::HostState::Paused) {
                setStatus(Paused, QStringLiteral("%1 paused the feed").arg(m_hostLabel));
            } else if (everLive && status() != Live) {
                // Resumed; the comp may simply be unchanged, so no frame yet.
                m_liveSinceMs.store(steadyMs(), std::memory_order_release);
                setStatus(Live, {});
            }
            if (!interruptibleSleep(kFramePollMs)) break;
            continue;
        }

        // Only what the Transmit device publishes is accepted: top-down
        // RGBA16F, RGBA order. Anything else is a protocol surprise — say so
        // rather than reinterpret bytes.
        const bool ok = d.pixel_format == qcbae::PixelFormat::RGBA16F
                     && d.channel_order == qcbae::ChannelOrder::RGBA
                     && d.width > 0 && d.height > 0
                     && d.bytes_per_row >= d.width * 8u;
        if (!ok) {
            ring.release();
            qWarning("HostBridgeSource: unexpected frame (format %u, order %u, %ux%u, row %u) — dropped",
                     unsigned(d.pixel_format), unsigned(d.channel_order),
                     d.width, d.height, d.bytes_per_row);
            continue;
        }

        const int w = int(d.width), h = int(d.height);
        QImage *img = takePoolImage(w, h);
        const auto *src = static_cast<const uint8_t *>(pixels);
        const size_t rowBytes = size_t(w) * 8u;
        for (int y = 0; y < h; ++y)
            std::memcpy(img->scanLine(y), src + size_t(y) * d.bytes_per_row, rowBytes);
        ring.release();

        // Microseconds, as the SRT source passes. The device sends the host's
        // time when it has one (Premiere) and -1 for "immediate" (AE's viewer).
        const int64_t ptsUs = (d.time_value >= 0 && d.time_scale > 0)
            ? int64_t(double(d.time_value) * 1e6 / double(d.time_scale))
            : int64_t(steadyMs() - sessionStartMs) * 1000;

        if (VideoDecoder *sink = m_sink) {
            sink->publishExternalFrame(FrameHandle::cpu(*img, ptsUs), ptsUs);
        }
        m_framesReceived.fetch_add(1, std::memory_order_acq_rel);
        m_bytesReceived.fetch_add(qint64(rowBytes) * h, std::memory_order_acq_rel);
        setGeometry(w, h);
        setNonFinite((d.flags & (qcbae::kFlagHasInf | qcbae::kFlagHasNaN)) != 0);

        if (!everLive || status() != Live) {
            everLive = true;
            m_liveSinceMs.store(steadyMs(), std::memory_order_release);
            setStatus(Live, {});
        }
        if (!loggedFirst) {
            loggedFirst = true;
            // The consumed property, logged: what arrived, and one sample
            // (centre pixel, as half-float bits) to compare with
            // `qcbae-probe dump` on the same ring.
            const auto *centre = reinterpret_cast<const uint16_t *>(
                img->constScanLine(h / 2)) + size_t(w / 2) * 4u;
            qInfo("HostBridgeSource: LIVE — first frame seq %llu %dx%d RGBA16F row %u flags 0x%x, "
                  "centre (%d,%d) RGBA half bits %04x %04x %04x %04x",
                  (unsigned long long)lastSeen, w, h, d.bytes_per_row, d.flags, w / 2, h / 2,
                  centre[0], centre[1], centre[2], centre[3]);
        }

        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lk(m_frameCbMutex);
            cb = m_frameCb;
        }
        if (cb) cb();
    }
}

// ---- cross-thread state publication ------------------------------

void HostBridgeSource::setStatus(Status s, const QString &detail)
{
    bool detailChanged;
    {
        std::lock_guard<std::mutex> lk(m_detailMutex);
        detailChanged = m_detail != detail;
        m_detail = detail;
    }
    const bool statusChanged_ = m_status.exchange(s, std::memory_order_acq_rel) != s;
    if (!statusChanged_ && !detailChanged) return;
    if (statusChanged_ && s != Live) m_liveSinceMs.store(0, std::memory_order_release);
    // Queue the emit onto the object's (GUI) thread — worker-side direct
    // emits would run QML handlers on this thread.
    QMetaObject::invokeMethod(this, [this] { emit statusChanged(); }, Qt::QueuedConnection);
}

void HostBridgeSource::setGeometry(int w, int h)
{
    const bool changed = m_width.exchange(w, std::memory_order_acq_rel) != w
                       | (m_height.exchange(h, std::memory_order_acq_rel) != h);
    if (changed)
        QMetaObject::invokeMethod(this, [this] { emit metadataChanged(); }, Qt::QueuedConnection);
}

void HostBridgeSource::setNonFinite(bool v)
{
    if (m_nonFinite.exchange(v, std::memory_order_acq_rel) != v)
        QMetaObject::invokeMethod(this, [this] { emit nonFiniteChanged(); }, Qt::QueuedConnection);
}

} // namespace qcv
