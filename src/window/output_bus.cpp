#include "output_bus.h"

#include "render/iplayer_renderer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qcv {

namespace {

// The largest frame a sink may ask for: the canvas cap in the wire format
// P2 serves. The ring is sized once; a request above it is refused.
uint64_t ringCapacityBytes()
{
    return qcbae::output_frame_bytes(qcbae::kOutputMaxWidth, qcbae::kOutputMaxHeight,
                                     qcbae::OutputPixelFormat::BGRA8);
}

// fps as a rational the wire can carry: the broadcast rates exactly, the
// rest to a thousandth.
void fpsToRational(double fps, uint32_t *num, uint32_t *den)
{
    *num = 0; *den = 0;
    if (!(fps > 0.0)) return;
    struct { double f; uint32_t n, d; } known[] = {
        { 23.976, 24000, 1001 }, { 29.97, 30000, 1001 }, { 59.94, 60000, 1001 },
        { 47.952, 48000, 1001 }, { 119.88, 120000, 1001 },
    };
    for (const auto &k : known) {
        if (std::fabs(fps - k.f) < 0.002) { *num = k.n; *den = k.d; return; }
    }
    if (std::fabs(fps - std::round(fps)) < 1e-6) { *num = static_cast<uint32_t>(std::lround(fps)); *den = 1; return; }
    *num = static_cast<uint32_t>(std::lround(fps * 1000.0)); *den = 1000;
}

QStringList manifestDirs()
{
    QStringList dirs;
    const QString env = qEnvironmentVariable("QCV_OUTPUT_MANIFEST_DIR");
    if (!env.isEmpty()) dirs << env;
#if defined(Q_OS_MACOS)
    dirs << QStringLiteral("/Library/Application Support/QCView/Outputs");
    dirs << QDir::homePath() + QStringLiteral("/Library/Application Support/QCView/Outputs");
#elif defined(Q_OS_WIN)
    const QString pd = qEnvironmentVariable("ProgramData");
    if (!pd.isEmpty()) dirs << pd + QStringLiteral("\\QCView\\Outputs");
    dirs << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Outputs");
#else
    dirs << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Outputs");
#endif
    return dirs;
}

} // namespace

OutputBus::OutputBus(QObject *parent)
    : QObject(parent)
{
    for (auto &s : m_audioSlots) s.store(nullptr, std::memory_order_relaxed);
    QSettings s;
    m_feedMuted.store(s.value(QStringLiteral("output/feedMuted"), false).toBool(), std::memory_order_release);
    m_includeGuides      = s.value(QStringLiteral("output/includeGuides"), true).toBool();
    m_includeAnnotations = s.value(QStringLiteral("output/includeAnnotations"), true).toBool();
    scanManifests();
    m_poll.setInterval(250);
    connect(&m_poll, &QTimer::timeout, this, &OutputBus::poll);
}

OutputBus::~OutputBus()
{
    rendererGone();
}

// ---- manifests ------------------------------------------------------------------

void OutputBus::scanManifests()
{
    for (const QString &dir : manifestDirs()) {
        const QDir d(dir);
        if (!d.exists()) continue;
        for (const QFileInfo &fi : d.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name)) {
            loadManifest(fi.absoluteFilePath());
        }
    }
    if (m_outputs.empty()) {
        qInfo("OutputBus: no output plugins registered (%s)",
              qPrintable(manifestDirs().join(QStringLiteral(", "))));
    }
}

void OutputBus::loadManifest(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject j = QJsonDocument::fromJson(f.readAll()).object();
    const QString id = j.value(QStringLiteral("id")).toString();
    if (qcbae::OutputRing::name_for(id.toStdString()).empty()) {
        qWarning("OutputBus: %s: id '%s' is not a plain [a-z0-9-] token of <= 13 bytes; skipped",
                 qPrintable(path), qPrintable(id));
        return;
    }
    if (find(id)) return;   // first registration wins (system before user)
    auto o = std::make_unique<Output>();
    o->id       = id;
    o->name     = j.value(QStringLiteral("name")).toString(id);
    o->exec     = j.value(QStringLiteral("exec")).toString();
    o->protocol = j.value(QStringLiteral("protocol")).toInt(1);
    for (const auto &a : j.value(QStringLiteral("args")).toArray()) o->args << a.toString();
    o->installed = !o->exec.isEmpty() && QFileInfo::exists(o->exec);
    o->plugin = o->installed ? QStringLiteral("installed") : QStringLiteral("not installed");
    qInfo("OutputBus: output '%s' (%s) from %s%s", qPrintable(o->id), qPrintable(o->name),
          qPrintable(path), o->installed ? "" : " — executable missing");
    m_outputs.push_back(std::move(o));
}

OutputBus::Output *OutputBus::find(const QString &id)
{
    for (auto &o : m_outputs) if (o->id == id) return o.get();
    return nullptr;
}

void OutputBus::rescan()
{
    // New manifests only; enabled outputs keep running.
    scanManifests();
    emit outputsChanged();
}

// ---- lifecycle ---------------------------------------------------------------------

void OutputBus::rendererReady()
{
    m_rendererReady = true;
    restoreEnabled();
    pushTargets();
    if (enabledCount() > 0) m_poll.start();
}

void OutputBus::rendererGone()
{
    m_poll.stop();
    for (auto &o : m_outputs) if (o->enabled) disable(*o);
    m_rendererReady = false;
}

void OutputBus::restoreEnabled()
{
    QSettings s;
    for (auto &o : m_outputs) {
        if (s.value(QStringLiteral("output/enabled/") + o->id, false).toBool() && !o->enabled) enable(*o);
    }
}

bool OutputBus::isEnabled(const QString &id) const
{
    for (const auto &o : m_outputs) if (o->id == id) return o->enabled;
    return false;
}

void OutputBus::setEnabled(const QString &id, bool on)
{
    Output *o = find(id);
    if (!o) return;
    QSettings s;
    s.setValue(QStringLiteral("output/enabled/") + id, on);
    if (on == o->enabled) return;
    if (on) enable(*o); else disable(*o);
    if (enabledCount() > 0) m_poll.start(); else m_poll.stop();
    emit outputsChanged();
}

void OutputBus::enable(Output &o)
{
    if (!m_rendererReady) { o.enabled = true; return; }   // picked up by rendererReady
    const std::string name = qcbae::OutputRing::name_for(o.id.toStdString());
    o.ring = std::make_unique<qcbae::OutputRing>();
    const bool willLaunch = o.installed;
    if (!o.ring->create(name, ringCapacityBytes(), qcbae::kDefaultSlots,
                        willLaunch ? qcbae::kOutputLaunchedByProducer : 0u)) {
        qWarning("OutputBus: %s: ring create failed: %s", qPrintable(o.id), o.ring->error().c_str());
        o.ring.reset();
        o.status = QStringLiteral("ring could not be created");
        return;
    }
    o.audio = std::make_unique<qcbae::AudioRing>();
    if (o.audio->create(qcbae::OutputRing::audio_name_for(o.id.toStdString()), 1000000000LL,
                        qcbae::kDefaultAudioSlots, qcbae::kDefaultAudioSlotFrames, 2)) {
        qcbae::AudioSession session {};
        session.sample_rate = 48000;
        session.channels    = 2;
        session.speed       = 1.0f;
        o.audio->begin_session(session);
        for (int i = 0; i < kMaxAudio; ++i) {
            qcbae::AudioRing *expect = nullptr;
            if (m_audioSlots[i].compare_exchange_strong(expect, o.audio.get(), std::memory_order_acq_rel)) {
                o.audioSlot = i;
                break;
            }
        }
    } else {
        qWarning("OutputBus: %s: audio ring create failed: %s", qPrintable(o.id), o.audio->error().c_str());
        o.audio.reset();
    }
    o.target = std::make_unique<output::OutputTarget>(o.ring.get());
    o.enabled  = true;
    o.accepted = false;
    o.present  = false;
    o.reqGen   = 0;
    o.statusGen = 0;
    o.restarts = 0;
    o.gaveUp   = false;
    o.status   = QStringLiteral("waiting for the sink");
    o.shape.clear();
    qInfo("OutputBus: %s enabled — rings %s, %s", qPrintable(o.id), name.c_str(),
          o.audio ? "audio ring beside it" : "no audio ring");
    pushTargets();
    if (willLaunch) launch(o);
    else o.plugin = o.installed ? QStringLiteral("installed") : QStringLiteral("not installed; waiting for a sink");
}

void OutputBus::disable(Output &o)
{
    o.enabled = false;
    // O13: retiring the rings is how a sink we started learns to quit, and
    // how a user-started one goes back to waiting.
    if (o.ring)  o.ring->set_state(qcbae::OutputState::Retired);
    if (o.audio) o.audio->set_state(qcbae::AudioState::Retired);
    // The renderer stops writing (synchronous handshake), the audio
    // thread stops seeing the ring, then both rings go.
    pushTargets();
    if (o.audioSlot >= 0) {
        m_audioSlots[o.audioSlot].store(nullptr, std::memory_order_release);
        o.audioSlot = -1;
        // A push that started before the slot cleared finishes in well
        // under a millisecond; give it that and more before unmapping.
        QThread::msleep(30);
    }
    o.target.reset();
    o.audio.reset();
    o.ring.reset();
    o.accepted = false;
    o.present  = false;
    o.status.clear();
    o.shape.clear();
    if (o.proc) {
        // It has its cue (the retired rings) and quits on its own. Hand the
        // QProcess over to itself so neither this disable nor QCView's exit
        // destroys it mid-quit — ~QProcess kills its child, which showed up
        // as "Destroyed while process is still running" at QCView quit. If
        // it ignores the cue, end it after a grace period.
        QProcess *p = o.proc;
        o.proc = nullptr;
        p->disconnect(this);
        p->setParent(nullptr);
        connect(p, &QProcess::finished, p, &QObject::deleteLater);
        if (o.launchedByUs) {
            QTimer::singleShot(3000, p, [p] { if (p->state() != QProcess::NotRunning) p->terminate(); });
        }
    }
    o.plugin = o.installed ? QStringLiteral("installed") : QStringLiteral("not installed");
    qInfo("OutputBus: %s disabled", qPrintable(o.id));
}

void OutputBus::launch(Output &o)
{
    if (!o.installed) return;
    if (o.proc) { o.proc->deleteLater(); o.proc = nullptr; }
    auto *p = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QCVO_ID"), o.id);
    p->setProcessEnvironment(env);
    p->setProgram(o.exec);
    p->setArguments(o.args);
    p->setProcessChannelMode(QProcess::ForwardedChannels);
    Output *op = &o;
    connect(p, &QProcess::finished, this, [this, op, p](int code, QProcess::ExitStatus st) {
        if (op->proc == p) onProcessFinished(op, code, st);
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [op, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            qWarning("OutputBus: %s: %s failed to start", qPrintable(op->id), qPrintable(op->exec));
            op->plugin = QStringLiteral("failed to start");
        }
    });
    o.proc = p;
    o.launchedByUs = true;
    o.plugin = QStringLiteral("starting");
    p->start();
    qInfo("OutputBus: %s: started %s %s", qPrintable(o.id), qPrintable(o.exec),
          qPrintable(o.args.join(QLatin1Char(' '))));
}

void OutputBus::onProcessFinished(Output *o, int code, QProcess::ExitStatus st)
{
    o->proc = nullptr;
    const bool crashed = st == QProcess::CrashExit;
    qInfo("OutputBus: %s: sink %s (%d)", qPrintable(o->id), crashed ? "crashed" : "exited", code);
    if (!o->enabled || !o->ring) { o->plugin = QStringLiteral("installed"); emit outputsChanged(); return; }
    // O13: we started it and its rings are still live — bring it back,
    // three times a minute at most, then report and stop.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (o->firstRestartMs == 0 || now - o->firstRestartMs > 60000) { o->firstRestartMs = now; o->restarts = 0; }
    if (o->restarts < 3 && !o->gaveUp) {
        ++o->restarts;
        o->plugin = QStringLiteral("restarting (%1)").arg(o->restarts);
        QTimer::singleShot(500 * o->restarts, this, [this, o] { if (o->enabled && o->ring) launch(*o); });
    } else {
        o->gaveUp = true;
        o->plugin = QStringLiteral("exited; not restarting (three in a minute)");
    }
    emit outputsChanged();
}

void OutputBus::pushTargets()
{
    IPlayerRenderer *r = m_renderer ? m_renderer() : nullptr;
    if (!r) return;
    std::vector<output::OutputTarget *> targets;
    for (auto &o : m_outputs) if (o->enabled && o->target) targets.push_back(o->target.get());
    r->setOutputTargets(targets);
}

// ---- the handshake, polled ---------------------------------------------------------

void OutputBus::poll()
{
    bool changed = false;
    uint32_t fpsNum = 0, fpsDen = 0;
    fpsToRational(m_fps ? m_fps() : 0.0, &fpsNum, &fpsDen);
    const int64_t now = qcbae::OutputRing::now_ns();
    for (auto &op : m_outputs) {
        Output &o = *op;
        if (!o.enabled || !o.ring || !o.target) continue;
        o.target->setSourceFps(fpsNum, fpsDen);
        if (o.ring->read_request(&o.req, &o.reqGen)) { answer(o); changed = true; }
        const bool present = o.ring->consumer_present(now);
        if (present != o.present) {
            o.present = present;
            changed = true;
            if (present) {
                o.plugin = o.launchedByUs ? QStringLiteral("running") : QStringLiteral("attached (user-started)");
                qInfo("OutputBus: %s: sink attached (pid %u)", qPrintable(o.id), o.ring->consumer_pid());
            } else {
                qInfo("OutputBus: %s: sink gone", qPrintable(o.id));
                if (!o.proc) o.plugin = o.installed ? QStringLiteral("installed") : QStringLiteral("not installed");
            }
        }
        o.target->setActive(present && o.accepted);
        std::string status;
        if (o.ring->read_status(&status, &o.statusGen)) {
            o.status = QString::fromStdString(status);
            changed = true;
        }
    }
    if (changed) emit outputsChanged();
}

void OutputBus::answer(Output &o)
{
    const qcbae::OutputRequest &r = o.req;
    qcbae::OutputReply reply {};
    auto refuse = [&](qcbae::OutputRefusal why, const char *msg) {
        reply.accepted = 0;
        reply.refusal  = static_cast<uint32_t>(why);
        std::snprintf(reply.message, sizeof reply.message, "%s", msg);
    };
    const auto fmt = static_cast<qcbae::OutputPixelFormat>(r.pixel_format);
    if (r.protocol_version != qcbae::kOutputDescVersion) {
        refuse(qcbae::OutputRefusal::VersionMismatch, "this QCView speaks output protocol 1");
    } else if (r.width == 0 || r.height == 0
               || r.width > qcbae::kOutputMaxWidth || r.height > qcbae::kOutputMaxHeight) {
        refuse(qcbae::OutputRefusal::TooLarge, "above the 3840x2160 canvas cap");
    } else if (fmt != qcbae::OutputPixelFormat::BGRA8) {
        refuse(qcbae::OutputRefusal::UnsupportedFormat, "BGRA8 only for now; RGBA16F comes with the HDR transfers");
    } else if (static_cast<qcbae::OutputTransfer>(r.transfer) != qcbae::OutputTransfer::SdrRec709) {
        refuse(qcbae::OutputRefusal::UnsupportedTransfer, "SDR Rec.709 only for now");
    } else if (r.fit != 0 && static_cast<qcbae::OutputFit>(r.fit) != qcbae::OutputFit::Letterbox) {
        refuse(qcbae::OutputRefusal::UnsupportedFormat, "letterbox fit only for now");
    } else if (r.content > static_cast<uint32_t>(qcbae::OutputContent::SideB)) {
        refuse(qcbae::OutputRefusal::UnsupportedContent, "content must be composite, A or B");
    } else {
        output::OutputSpec spec;
        spec.width    = r.width;
        spec.height   = r.height;
        spec.format   = fmt;
        spec.transfer = qcbae::OutputTransfer::SdrRec709;
        spec.fit      = qcbae::OutputFit::Letterbox;
        spec.content  = r.content == 0 ? qcbae::OutputContent::Composite
                                       : static_cast<qcbae::OutputContent>(r.content);
        spec.layers   = r.layers & ((m_includeGuides ? qcbae::kOutputLayerGuides : 0u)
                                    | (m_includeAnnotations ? qcbae::kOutputLayerAnnotations : 0u));
        o.target->setSpec(spec);
        reply.accepted     = 1;
        reply.width        = spec.width;
        reply.height       = spec.height;
        reply.pixel_format = static_cast<uint32_t>(spec.format);
        reply.transfer     = static_cast<uint32_t>(spec.transfer);
        reply.content      = static_cast<uint32_t>(spec.content);
        reply.layers       = spec.layers;
        std::snprintf(reply.message, sizeof reply.message, "%ux%u BGRA8 SDR from QCView", spec.width, spec.height);
        o.shape = QStringLiteral("%1x%2 BGRA8").arg(spec.width).arg(spec.height);
    }
    o.accepted = reply.accepted != 0;
    o.ring->write_reply(reply, o.reqGen);
    qInfo("OutputBus: %s: request %llu (%ux%u fmt %u transfer %u content %u layers 0x%x) -> %s \"%s\"",
          qPrintable(o.id), static_cast<unsigned long long>(o.reqGen), r.width, r.height, r.pixel_format,
          r.transfer, r.content, r.layers, o.accepted ? "accepted" : "refused", reply.message);
    if (!o.accepted) o.shape = QString::fromUtf8(reply.message);
}

// ---- settings ----------------------------------------------------------------------

void OutputBus::setFeedMuted(bool on)
{
    if (on == feedMuted()) return;
    m_feedMuted.store(on, std::memory_order_release);
    QSettings s;
    s.setValue(QStringLiteral("output/feedMuted"), on);
    emit feedMutedChanged();
}

void OutputBus::setIncludeGuides(bool on)
{
    if (on == m_includeGuides) return;
    m_includeGuides = on;
    QSettings s;
    s.setValue(QStringLiteral("output/includeGuides"), on);
    // Re-answer every accepted request with the new layer set.
    for (auto &o : m_outputs) if (o->enabled && o->ring && o->accepted) answer(*o);
    emit layersChanged();
}

void OutputBus::setIncludeAnnotations(bool on)
{
    if (on == m_includeAnnotations) return;
    m_includeAnnotations = on;
    QSettings s;
    s.setValue(QStringLiteral("output/includeAnnotations"), on);
    for (auto &o : m_outputs) if (o->enabled && o->ring && o->accepted) answer(*o);
    emit layersChanged();
}

QVariantList OutputBus::outputs() const
{
    QVariantList list;
    for (const auto &o : m_outputs) {
        QVariantMap m;
        m.insert(QStringLiteral("id"),        o->id);
        m.insert(QStringLiteral("name"),      o->name);
        m.insert(QStringLiteral("installed"), o->installed);
        m.insert(QStringLiteral("enabled"),   o->enabled);
        m.insert(QStringLiteral("present"),   o->present);
        m.insert(QStringLiteral("accepted"),  o->accepted);
        m.insert(QStringLiteral("shape"),     o->shape);
        m.insert(QStringLiteral("status"),    o->status);
        m.insert(QStringLiteral("plugin"),    o->plugin);
        m.insert(QStringLiteral("published"), static_cast<qulonglong>(o->ring ? o->ring->newest_seq() : 0u));
        m.insert(QStringLiteral("seen"),      static_cast<qulonglong>(o->ring ? o->ring->consumer_last_seen() : 0u));
        list.push_back(m);
    }
    return list;
}

int OutputBus::enabledCount() const
{
    int n = 0;
    for (const auto &o : m_outputs) if (o->enabled) ++n;
    return n;
}

// ---- audio ----------------------------------------------------------------------------

void OutputBus::audioTap(void *ctx, const float *stereo, uint32_t frames, bool muted)
{
    static_cast<OutputBus *>(ctx)->tap(stereo, frames, muted);
}

void OutputBus::tap(const float *stereo, uint32_t frames, bool muted)
{
    // Device thread: fixed buffers, no locks, no allocation.
    constexpr uint32_t kChunk = 1024;
    thread_local float planes[2][kChunk];
    const bool silent = muted || m_feedMuted.load(std::memory_order_acquire);
    const uint32_t flags = silent ? qcbae::kAudioPacketMuted : 0u;
    const int64_t now = qcbae::OutputRing::now_ns();
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(kChunk, frames - done);
        if (silent) {
            std::memset(planes[0], 0, n * sizeof(float));
            std::memset(planes[1], 0, n * sizeof(float));
        } else {
            const float *src = stereo + static_cast<size_t>(done) * 2;
            for (uint32_t i = 0; i < n; ++i) { planes[0][i] = src[i * 2]; planes[1][i] = src[i * 2 + 1]; }
        }
        const float *p[2] = { planes[0], planes[1] };
        const int64_t t = now + static_cast<int64_t>(done) * 1000000000LL / 48000;
        for (auto &slot : m_audioSlots) {
            if (qcbae::AudioRing *ring = slot.load(std::memory_order_acquire)) {
                ring->push(p, 2, 48000, n, t, flags);
            }
        }
        done += n;
    }
}

} // namespace qcv
