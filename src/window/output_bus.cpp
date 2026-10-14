#include "output_bus.h"
#include "output/stream_muxer.h"

#include "render/iplayer_renderer.h"
#include "color/ocio_config_manager.h"

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
#include <QUrl>
#include <QUrlQuery>
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
    m_streamAvailable = output::makePlatformVideoEncoder() != nullptr;
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
    o->label    = j.value(QStringLiteral("label")).toString();
    o->product  = j.value(QStringLiteral("product")).toString();
    {
        // Older manifests (QCBridgeN 0.0.2, "QCBridgeN (NDI)") carry only
        // the name: the product is what precedes the parenthesis, the label
        // the first word inside it ("QCBridgeA (AJA SDI/HDMI)" -> "AJA").
        const int open = o->name.indexOf(QLatin1Char('('));
        if (o->product.isEmpty())
            o->product = (open > 0 ? o->name.left(open) : QString()).trimmed();
        if (o->label.isEmpty()) {
            if (open >= 0) {
                const int close = o->name.indexOf(QLatin1Char(')'), open);
                const QString inner = o->name.mid(open + 1, close > open ? close - open - 1 : -1).trimmed();
                o->label = inner.section(QLatin1Char(' '), 0, 0);
            }
            if (o->label.isEmpty()) o->label = o->name;
        }
    }
    o->exec     = j.value(QStringLiteral("exec")).toString();
    o->rasterSettable = j.value(QStringLiteral("raster")).toString() == QLatin1String("auto");
    o->protocol = j.value(QStringLiteral("protocol")).toInt(1);
    for (const auto &a : j.value(QStringLiteral("args")).toArray()) o->args << a.toString();
    o->installed = !o->exec.isEmpty() && QFileInfo::exists(o->exec);
    o->plugin = o->installed ? QStringLiteral("installed") : QStringLiteral("not installed");
    {
        QSettings s;
        const QString k = QStringLiteral("output/") + id + QLatin1Char('/');
        o->raster    = std::clamp(s.value(k + QStringLiteral("raster"), 0).toInt(), 0, 2);
        o->transfer  = std::clamp(s.value(k + QStringLiteral("transfer"), 1).toInt(), 1, 3);
        o->primaries = std::clamp(s.value(k + QStringLiteral("primaries"), 1).toInt(), 1, 2);
        o->peakNits  = s.value(k + QStringLiteral("peakNits"), 1000.0).toDouble();
        o->minNits   = s.value(k + QStringLiteral("minNits"), 0.005).toDouble();
        o->maxCll    = s.value(k + QStringLiteral("maxCll"), 1000).toInt();
        o->maxFall   = s.value(k + QStringLiteral("maxFall"), 400).toInt();
    }
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
    // Dev aids: QCV_STREAM_SPIKE=srt://... starts a stream with the
    // renderer (an empty value: from the saved settings, as Start would);
    // QCV_STREAM_SPIKE_CYCLES=<n> stops and restarts it every 3 s, n times.
    if (qEnvironmentVariableIsSet("QCV_STREAM_SPIKE")) {
        if (qEnvironmentVariable("QCV_STREAM_SPIKE").isEmpty()) startStream();
        else startSpikeStream();
        const int cycles = qEnvironmentVariable("QCV_STREAM_SPIKE_CYCLES").toInt();
        if (cycles > 0) {
            auto *t = new QTimer(this);
            t->setInterval(3000);
            connect(t, &QTimer::timeout, this, [this, t, cycles]() mutable {
                static int done = 0;
                if (m_stream) { qInfo("OutputBus: cycle %d/%d: stop", done + 1, cycles); stopStream(); }
                else {
                    ++done;
                    if (done >= cycles) { qInfo("OutputBus: cycles done"); t->stop(); t->deleteLater(); return; }
                    qInfo("OutputBus: cycle %d/%d: start", done + 1, cycles);
                    if (qEnvironmentVariable("QCV_STREAM_SPIKE").isEmpty()) startStream(); else startSpikeStream();
                }
            });
            t->start();
        }
    }
    pushTargets();
    if (enabledCount() > 0 || m_stream) m_poll.start();
}

void OutputBus::rendererGone()
{
    m_poll.stop();
    for (auto &o : m_outputs) if (o->enabled) disable(*o);
    teardownStream();
    m_rendererReady = false;
}

// ---- the stream output -----------------------------------------------------------------

namespace {
const char *kStreamKeys[] = { "url", "key", "bitrateKbps", "resolution", "srtLatencyMs", "profile", "audioKbps", "fpsMode" };
QVariant streamDefault(const QString &key)
{
    if (key == QLatin1String("bitrateKbps"))  return 6000;
    if (key == QLatin1String("resolution"))   return QStringLiteral("1080p");
    if (key == QLatin1String("srtLatencyMs")) return 120;
    if (key == QLatin1String("profile"))      return QStringLiteral("baseline");
    if (key == QLatin1String("audioKbps"))    return 160;
    if (key == QLatin1String("fpsMode"))      return QStringLiteral("auto");
    return QString();
}
} // namespace

output::StreamConfig OutputBus::streamConfigFromSettings() const
{
    QSettings s;
    auto v = [&](const char *k) { return s.value(QStringLiteral("output/stream/") + QLatin1String(k), streamDefault(QLatin1String(k))); };
    output::StreamConfig cfg;
    cfg.url = v("url").toString().trimmed().toStdString();
    const QString res = v("resolution").toString();
    if (res == QLatin1String("720p"))       { cfg.width = 1280; cfg.height = 720; }
    else if (res == QLatin1String("540p"))  { cfg.width = 960;  cfg.height = 540; }
    else if (res == QLatin1String("2160p")) { cfg.width = 3840; cfg.height = 2160; }
    else                                    { cfg.width = 1920; cfg.height = 1080; }
    cfg.bitrateKbps  = std::clamp(v("bitrateKbps").toUInt(), 500u, 50000u);
    cfg.srtLatencyMs = std::clamp(v("srtLatencyMs").toInt(), 20, 5000);
    cfg.profile      = v("profile").toString() == QLatin1String("main") ? output::H264Profile::Main
                                                                          : output::H264Profile::Baseline;
    cfg.audioKbps    = v("audioKbps").toUInt();
    const QString fpsMode = v("fpsMode").toString();
    if (fpsMode != QLatin1String("auto")) fpsToRational(fpsMode.toDouble(), &cfg.fpsNum, &cfg.fpsDen);
    // The key: for SRT it is the stream id, unless the URL already carries
    // one; for RTMP it is the last path element (rtmp://host/app/<key>).
    const QString key = v("key").toString().trimmed();
    if (!key.isEmpty()) {
        if (output::StreamMuxer::isRtmp(cfg.url)) {
            std::string url = cfg.url;
            while (!url.empty() && url.back() == '/') url.pop_back();
            cfg.url = url + "/" + key.toStdString();
        } else {
            const QUrl u(QString::fromStdString(cfg.url));
            const QUrlQuery q(u.query());
            if (!q.hasQueryItem(QStringLiteral("streamid"))) cfg.streamId = key.toStdString();
        }
    }
    return cfg;
}

bool OutputBus::startStreamWith(const output::StreamConfig &cfg, std::string *error)
{
    teardownStream();
    if (!m_rendererReady) { if (error) *error = "no renderer yet"; return false; }
    auto sink = std::make_unique<output::StreamSink>(output::makePlatformVideoEncoder());
    if (!sink->start(cfg, error)) return false;
    m_stream = std::move(sink);
    m_streamTarget = std::make_unique<output::OutputTarget>(m_stream.get());
    output::OutputSpec spec;
    spec.width = cfg.width;  spec.height = cfg.height;
    spec.format = qcbae::OutputPixelFormat::BGRA8;
    spec.transfer = qcbae::OutputTransfer::SdrRec709;
    spec.content = qcbae::OutputContent::Composite;
    spec.fit = qcbae::OutputFit::Letterbox;
    spec.layers = (m_includeGuides ? qcbae::kOutputLayerGuides : 0u)
                | (m_includeAnnotations ? qcbae::kOutputLayerAnnotations : 0u);
    m_streamTarget->setSpec(spec);
    m_streamTarget->setActive(true);
    uint32_t fpsNum = 0, fpsDen = 0;
    fpsToRational(m_fps ? m_fps() : 0.0, &fpsNum, &fpsDen);
    m_stream->setSourceFps(fpsNum, fpsDen);
    m_streamTarget->setSourceFps(fpsNum, fpsDen);
    m_streamTap.store(m_stream.get(), std::memory_order_release);
    pushTargets();
    m_poll.start();
    m_streamError.clear();
    qInfo("OutputBus: stream -> %s at %ux%u, %u kbps, audio %u kbps", cfg.url.c_str(), cfg.width, cfg.height,
          cfg.bitrateKbps, cfg.audioKbps);
    emit streamChanged();
    return true;
}

void OutputBus::startSpikeStream()
{
    output::StreamConfig cfg = streamConfigFromSettings();
    cfg.url = qEnvironmentVariable("QCV_STREAM_SPIKE").toStdString();
    const QString size = qEnvironmentVariable("QCV_STREAM_SPIKE_SIZE");
    const QStringList wh = size.split(QLatin1Char('x'));
    if (wh.size() == 2) { cfg.width = wh[0].toUInt(); cfg.height = wh[1].toUInt(); }
    if (qEnvironmentVariableIsSet("QCV_STREAM_SPIKE_KBPS"))       cfg.bitrateKbps  = qEnvironmentVariable("QCV_STREAM_SPIKE_KBPS").toUInt();
    if (qEnvironmentVariableIsSet("QCV_STREAM_SPIKE_LATENCY_MS")) cfg.srtLatencyMs = qEnvironmentVariable("QCV_STREAM_SPIKE_LATENCY_MS").toInt();
    if (qEnvironmentVariableIsSet("QCV_STREAM_SPIKE_AUDIO_KBPS")) cfg.audioKbps    = qEnvironmentVariable("QCV_STREAM_SPIKE_AUDIO_KBPS").toUInt();
    if (qEnvironmentVariableIsSet("QCV_STREAM_SPIKE_STREAMID"))   cfg.streamId     = qEnvironmentVariable("QCV_STREAM_SPIKE_STREAMID").toStdString();
    if (qEnvironmentVariableIsSet("QCV_STREAM_SPIKE_PASSPHRASE")) cfg.passphrase   = qEnvironmentVariable("QCV_STREAM_SPIKE_PASSPHRASE").toStdString();
    if (qEnvironmentVariable("QCV_STREAM_SPIKE_PROFILE") == QLatin1String("main")) cfg.profile = output::H264Profile::Main;
    const QString fps = qEnvironmentVariable("QCV_STREAM_SPIKE_FPS");
    if (!fps.isEmpty()) {
        const QStringList nd = fps.split(QLatin1Char('/'));
        if (nd.size() == 2) { cfg.fpsNum = nd[0].toUInt(); cfg.fpsDen = nd[1].toUInt(); }
        else fpsToRational(fps.toDouble(), &cfg.fpsNum, &cfg.fpsDen);
    }
    std::string err;
    if (!startStreamWith(cfg, &err)) qWarning("OutputBus: stream spike not started: %s", err.c_str());
}

bool OutputBus::startStream()
{
    const output::StreamConfig cfg = streamConfigFromSettings();
    std::string err;
    if (cfg.url.empty()) err = "enter the server URL first";
    else if (!m_streamAvailable) err = "no hardware encoder on this platform yet";
    else if (startStreamWith(cfg, &err)) return true;
    m_streamError = QString::fromStdString(err);
    emit streamChanged();
    return false;
}

void OutputBus::stopStream()
{
    teardownStream();
    m_streamError.clear();   // the user's own stop is not an error, even mid-connect
    emit streamChanged();
}

void OutputBus::teardownStream()
{
    if (!m_stream) return;
    // The audio thread stops seeing the sink, the renderer stops using the
    // target (synchronous handshake), then the sink waits for the surfaces
    // still on the GPU and shuts its threads.
    m_streamTap.store(nullptr, std::memory_order_release);
    if (m_streamTarget) m_streamTarget->setActive(false);
    m_streamTarget.reset();
    pushTargets();
    m_stream->stop();
    m_stream.reset();
    if (enabledCount() == 0) m_poll.stop();
}

void OutputBus::setStreamSetting(const QString &key, const QVariant &value)
{
    bool known = false;
    for (const char *k : kStreamKeys) if (key == QLatin1String(k)) { known = true; break; }
    if (!known) return;
    QSettings s;
    s.setValue(QStringLiteral("output/stream/") + key, value);
    emit streamChanged();
}

QVariantMap OutputBus::stream() const
{
    QVariantMap m;
    QSettings s;
    for (const char *k : kStreamKeys) {
        m.insert(QLatin1String(k), s.value(QStringLiteral("output/stream/") + QLatin1String(k), streamDefault(QLatin1String(k))));
    }
    m.insert(QStringLiteral("available"), m_streamAvailable);
    m.insert(QStringLiteral("running"), m_stream != nullptr);
    m.insert(QStringLiteral("error"), m_streamError);
    if (m_stream) {
        const output::StreamSink::Status st = m_stream->status();
        m.insert(QStringLiteral("state"),       QString::fromStdString(st.state));
        m.insert(QStringLiteral("encoder"),     QString::fromStdString(st.encoder));
        m.insert(QStringLiteral("fps"),         st.fps);
        m.insert(QStringLiteral("kbps"),        st.kbps);
        m.insert(QStringLiteral("encoded"),     static_cast<qulonglong>(st.framesEncoded));
        m.insert(QStringLiteral("repeated"),    static_cast<qulonglong>(st.framesRepeated));
        m.insert(QStringLiteral("skipped"),     static_cast<qulonglong>(st.framesSkipped));
        m.insert(QStringLiteral("dropped"),     static_cast<qulonglong>(st.framesDropped));
        m.insert(QStringLiteral("audioSilent"), static_cast<qulonglong>(st.audioSilent));
        m.insert(QStringLiteral("muxErrors"),   static_cast<qulonglong>(st.muxErrors));
        m.insert(QStringLiteral("reconnects"),  static_cast<qulonglong>(st.reconnects));
    } else {
        m.insert(QStringLiteral("state"), m_streamError.isEmpty() ? QStringLiteral("off") : m_streamError);
        m.insert(QStringLiteral("encoder"), QString());
        m.insert(QStringLiteral("fps"), 0.0);
        m.insert(QStringLiteral("kbps"), 0.0);
    }
    return m;
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
    if (enabledCount() > 0 || m_stream) m_poll.start(); else m_poll.stop();
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
    if (m_streamTarget) targets.push_back(m_streamTarget.get());
    r->setOutputTargets(targets);
}

// ---- the handshake, polled ---------------------------------------------------------

void OutputBus::poll()
{
    bool changed = false;
    uint32_t fpsNum = 0, fpsDen = 0;
    fpsToRational(m_fps ? m_fps() : 0.0, &fpsNum, &fpsDen);
    const int64_t now = qcbae::OutputRing::now_ns();
    if (m_stream) {
        m_stream->setSourceFps(fpsNum, fpsDen);
        if (m_streamTarget) m_streamTarget->setSourceFps(fpsNum, fpsDen);
        if (m_stream->failed()) {
            // The session ended on its own: keep its last word as the status.
            m_streamError = QString::fromStdString(m_stream->status().state);
            teardownStream();
        }
        emit streamChanged();
    }
    for (auto &op : m_outputs) {
        Output &o = *op;
        if (!o.enabled || !o.ring || !o.target) continue;
        o.target->setSourceFps(fpsNum, fpsDen);
        if (o.ring->read_request(&o.req, &o.reqGen)) { answer(o); changed = true; }
        else if (o.accepted && o.req.width == 0 && o.req.height == 0) {
            // The sink left the raster to us: follow the source's class.
            const QSize want = rasterFor(o);
            uint64_t gen = 0;
            const output::OutputSpec cur = o.target->spec(&gen);
            if (uint32_t(want.width()) != cur.width || uint32_t(want.height()) != cur.height) {
                qInfo("OutputBus: %s: raster %ux%u -> %dx%d (the source changed)", qPrintable(o.id),
                      cur.width, cur.height, want.width(), want.height());
                answer(o); changed = true;
            }
        }
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
        const bool active = present && o.accepted;
        if (active && !o.target->active()) {
            // The pass publishes on change only, and on Windows nothing redraws
            // while the picture is paused: a sink that just attached would wait
            // for the next frame. Ask for one so it gets the picture now.
            o.target->setActive(true);
            if (IPlayerRenderer *r = m_renderer ? m_renderer() : nullptr) r->requestUpdate();
        } else {
            o.target->setActive(active);
        }
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
    // The transfer: the sink's wish, or (Unspecified) this output's setting.
    // It is what the wire is told and signals; the picture goes out as the
    // viewport shows it (OCIO's or minColor's display is set in the colour
    // panel, and a Premiere or After Effects feed that is already PQ or HLG
    // passes straight through). PQ adds the mastering metadata below.
    const uint32_t transfer = r.transfer != 0 ? r.transfer : static_cast<uint32_t>(o.transfer);
    const bool hdr = transfer != static_cast<uint32_t>(qcbae::OutputTransfer::SdrRec709);
    QString hdrWhy;
    if (transfer > static_cast<uint32_t>(qcbae::OutputTransfer::HlgRec2020)) hdrWhy = QStringLiteral("unknown transfer");
    // The raster: the sink's wish, or (0x0) this output's setting — 1080p, UHD,
    // or the source's class (UHD for anything above 1080p). A sink that asks
    // for 0x0 is re-answered from poll() when the source changes class.
    const bool autoRaster = r.width == 0 && r.height == 0;
    const QSize raster = autoRaster ? rasterFor(o) : QSize(int(r.width), int(r.height));
    if (r.protocol_version != qcbae::kOutputDescVersion) {
        refuse(qcbae::OutputRefusal::VersionMismatch, "this QCView speaks output protocol 1");
    } else if (raster.width() <= 0 || raster.height() <= 0
               || raster.width() > int(qcbae::kOutputMaxWidth) || raster.height() > int(qcbae::kOutputMaxHeight)) {
        refuse(qcbae::OutputRefusal::TooLarge, "above the 3840x2160 canvas cap");
    } else if (fmt != qcbae::OutputPixelFormat::BGRA8 && fmt != qcbae::OutputPixelFormat::RGB10) {
        refuse(qcbae::OutputRefusal::UnsupportedFormat, "BGRA8 or RGB10; RGBA16F comes with the HDR transfers");
    } else if (!hdrWhy.isEmpty()) {
        refuse(qcbae::OutputRefusal::UnsupportedTransfer, hdrWhy.toUtf8().constData());
    } else if (r.fit != 0 && static_cast<qcbae::OutputFit>(r.fit) != qcbae::OutputFit::Letterbox) {
        refuse(qcbae::OutputRefusal::UnsupportedFormat, "letterbox fit only for now");
    } else if (r.content > static_cast<uint32_t>(qcbae::OutputContent::SideB)) {
        refuse(qcbae::OutputRefusal::UnsupportedContent, "content must be composite, A or B");
    } else {
        output::OutputSpec spec;
        spec.width    = uint32_t(raster.width());
        spec.height   = uint32_t(raster.height());
        spec.format   = fmt;
        spec.transfer = static_cast<qcbae::OutputTransfer>(transfer);
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
        const char *fmtName = spec.format == qcbae::OutputPixelFormat::RGB10 ? "RGB10" : "BGRA8";
        const char *tfName  = transfer == 2 ? "PQ Rec.2100" : transfer == 3 ? "HLG Rec.2100" : "SDR Rec.709";
        std::snprintf(reply.message, sizeof reply.message, "%ux%u %s %s from QCView", spec.width, spec.height, fmtName, tfName);
        o.shape = QStringLiteral("%1x%2 %3 %4").arg(spec.width).arg(spec.height).arg(QLatin1String(fmtName), QLatin1String(tfName));
    }
    o.accepted = reply.accepted != 0;
    o.ring->write_reply(reply, o.reqGen);
    if (o.accepted) {
        writeHdrInfo(o, transfer);
        // The spec generation is part of the output pass's change stamp, so
        // the next draw re-publishes in the new shape; ask for that draw now
        // (on Windows nothing redraws while the picture is paused).
        if (IPlayerRenderer *rend = m_renderer ? m_renderer() : nullptr) rend->requestUpdate();
    }
    qInfo("OutputBus: %s: request %llu (%ux%u fmt %u transfer %u content %u layers 0x%x) -> %s \"%s\"%s",
          qPrintable(o.id), static_cast<unsigned long long>(o.reqGen), r.width, r.height, r.pixel_format,
          r.transfer, r.content, r.layers, o.accepted ? "accepted" : "refused", reply.message,
          hdr ? " [as displayed]" : "");
    if (!o.accepted) o.shape = QString::fromUtf8(reply.message);
}

QSize OutputBus::rasterFor(const Output &o) const
{
    if (o.raster == 1) return QSize(1920, 1080);
    if (o.raster == 2) return QSize(3840, 2160);
    const QSize src = m_sourceSize ? m_sourceSize() : QSize();
    return (src.width() > 1920 || src.height() > 1080) ? QSize(3840, 2160) : QSize(1920, 1080);
}

void OutputBus::writeHdrInfo(Output &o, uint32_t transfer)
{
    if (!o.ring) return;
    qcbae::OutputHdrInfo info {};
    info.transfer = transfer;
    if (transfer != static_cast<uint32_t>(qcbae::OutputTransfer::SdrRec709)) {
        info.primaries          = static_cast<uint32_t>(o.primaries);
        info.max_mastering_nits = static_cast<float>(o.peakNits);
        info.min_mastering_nits = static_cast<float>(o.minNits);
        info.max_cll            = static_cast<uint32_t>(std::max(0, o.maxCll));
        info.max_fall           = static_cast<uint32_t>(std::max(0, o.maxFall));
    }
    o.ring->write_hdr_info(info);
}

void OutputBus::setOutputSetting(const QString &id, const QString &key, const QVariant &value)
{
    Output *o = find(id);
    if (!o) return;
    if      (key == QLatin1String("raster"))    o->raster    = std::clamp(value.toInt(), 0, 2);
    else if (key == QLatin1String("transfer"))  o->transfer  = std::clamp(value.toInt(), 1, 3);
    else if (key == QLatin1String("primaries")) o->primaries = std::clamp(value.toInt(), 1, 2);
    else if (key == QLatin1String("peakNits"))  o->peakNits  = std::max(1.0, value.toDouble());
    else if (key == QLatin1String("minNits"))   o->minNits   = std::max(0.0, value.toDouble());
    else if (key == QLatin1String("maxCll"))    o->maxCll    = std::max(0, value.toInt());
    else if (key == QLatin1String("maxFall"))   o->maxFall   = std::max(0, value.toInt());
    else return;
    QSettings s;
    s.setValue(QStringLiteral("output/") + id + QLatin1Char('/') + key, value);
    if (o->enabled && o->ring && o->accepted) answer(*o);
    emit outputsChanged();
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
        m.insert(QStringLiteral("label"),     o->label);
        m.insert(QStringLiteral("product"),   o->product);
        m.insert(QStringLiteral("installed"), o->installed);
        m.insert(QStringLiteral("enabled"),   o->enabled);
        m.insert(QStringLiteral("present"),   o->present);
        m.insert(QStringLiteral("accepted"),  o->accepted);
        m.insert(QStringLiteral("raster"),    o->raster);
        // The raster dropdown is for sinks that leave the raster to QCView
        // (the manifest says so, or the live request did).
        m.insert(QStringLiteral("rasterSettable"), o->rasterSettable || (o->accepted && o->req.width == 0 && o->req.height == 0));
        m.insert(QStringLiteral("transfer"),  o->transfer);
        m.insert(QStringLiteral("primaries"), o->primaries);
        m.insert(QStringLiteral("peakNits"),  o->peakNits);
        m.insert(QStringLiteral("minNits"),   o->minNits);
        m.insert(QStringLiteral("maxCll"),    o->maxCll);
        m.insert(QStringLiteral("maxFall"),   o->maxFall);
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
    // Device thread: fixed buffers, no locks, no allocation. With no output
    // enabled there is no ring to feed and nothing is touched: the tap is
    // installed for the life of the player, the cost is only paid when on.
    output::StreamSink *stream = m_streamTap.load(std::memory_order_acquire);
    const bool silent = muted || m_feedMuted.load(std::memory_order_acquire);
    if (stream) stream->pushAudio(stereo, frames, silent);
    bool any = false;
    for (auto &slot : m_audioSlots) if (slot.load(std::memory_order_acquire)) { any = true; break; }
    if (!any) return;
    constexpr uint32_t kChunk = 1024;
    thread_local float planes[2][kChunk];
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
