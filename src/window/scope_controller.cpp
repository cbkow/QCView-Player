#include "scope_controller.h"

#include "color/ocio_config_manager.h"
#include "color/scope_names.h"
#include "project/project_manager.h"
#include "render/backdrop_image_provider.h"
#include "render/iplayer_renderer.h"

#include <QFileInfo>
#include <QLocale>
#include <QPainter>
#include <QSettings>
#include <QtLogging>

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cmath>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

namespace {

// Linear primaries → linear Rec.2020 (D65).
void rec709To2020(const float *in, float *out)
{
    out[0] = 0.6274039f * in[0] + 0.3292830f * in[1] + 0.0433131f * in[2];
    out[1] = 0.0690973f * in[0] + 0.9195404f * in[1] + 0.0113623f * in[2];
    out[2] = 0.0163914f * in[0] + 0.0880133f * in[1] + 0.8955953f * in[2];
}
void p3To2020(const float *in, float *out)
{
    out[0] =  0.7538330f * in[0] + 0.1985974f * in[1] + 0.0475696f * in[2];
    out[1] =  0.0457438f * in[0] + 0.9417772f * in[1] + 0.0124790f * in[2];
    out[2] = -0.0012103f * in[0] + 0.0176017f * in[1] + 0.9836086f * in[2];
}

QVariantMap point(float x, float y)
{
    QVariantMap m;
    m[QStringLiteral("x")] = x;
    m[QStringLiteral("y")] = y;
    return m;
}

// The bundled config the scopes read a file through when the live config
// has no colourspace for its tags (a studio config without PQ, say).
// Labelled "built-in config" on the badge.
// CONFIG UPGRADE: move this with the default config (assets/OCIO/patches/
// README.md, "Names QCView depends on").
constexpr const char *kScopeFallbackConfigDir = "Blender5.2";

// First of `names` the config knows (aliases resolve), else empty.
template <int N>
QString firstKnown(OCIO::ConstConfigRcPtr cfg, const char *const (&names)[N])
{
    if (!cfg) return {};
    for (const char *n : names) {
        if (cfg->getColorSpace(n)) return QString::fromUtf8(n);
    }
    return {};
}

QString canonicalName(OCIO::ConstConfigRcPtr cfg, const QString &name)
{
    if (!cfg || name.isEmpty()) return name;
    OCIO::ConstColorSpaceRcPtr cs = cfg->getColorSpace(name.toUtf8().constData());
    return cs ? QString::fromUtf8(cs->getName()) : name;
}

// Waveform HDR top of scale: 300, 600, 1 000, 2 000 or 4 000 nits.
int snapPeak(int nits)
{
    if (nits >= 4000) return 4000;
    if (nits >= 2000) return 2000;
    if (nits >= 1000) return 1000;
    return nits >= 600 ? 600 : 300;
}

} // namespace

ScopeController::ScopeController(OCIOConfigManager *ocio, ProjectManager *project,
                                 RendererFn renderer, QObject *parent)
    : QObject(parent), m_ocio(ocio), m_project(project), m_renderer(std::move(renderer))
{
    QSettings s;
    m_zoom        = std::clamp(s.value(QStringLiteral("scope/zoom"), 1).toInt(), 1, 4);
    m_colorize    = s.value(QStringLiteral("scope/colorize"), false).toBool();
    m_brightness  = std::clamp(s.value(QStringLiteral("scope/brightness"), 1.0).toDouble(), 0.1, 10.0);
    m_wavePeak    = snapPeak(s.value(QStringLiteral("scope/waveformPeak"), 1000).toInt());
    m_scaleMode   = std::clamp(s.value(QStringLiteral("scope/scaleMode"), 0).toInt(), 0, 2);

    m_pollTimer.setInterval(33);
    connect(&m_pollTimer, &QTimer::timeout, this, &ScopeController::poll);
    if (m_ocio) {
        connect(m_ocio, &OCIOConfigManager::activeChainChanged, this, &ScopeController::refresh);
        connect(m_ocio, &OCIOConfigManager::configChanged, this, &ScopeController::refresh);
    }
    if (m_project) {
        connect(m_project, &ProjectManager::activeItemIdChanged, this, &ScopeController::refresh);
        connect(m_project, &ProjectManager::bSourceChanged, this, &ScopeController::refresh);
    }
    refresh();
}

void ScopeController::setActive(bool on)
{
    if (m_active == on) return;
    m_active = on;
    if (m_active || m_waveActive) m_pollTimer.start(); else m_pollTimer.stop();
    refresh();
    emit activeChanged();
}

void ScopeController::setWaveformActive(bool on)
{
    if (m_waveActive == on) return;
    m_waveActive = on;
    if (m_active || m_waveActive) m_pollTimer.start(); else m_pollTimer.stop();
    refresh();
    emit activeChanged();
}

void ScopeController::setDualView(bool dual)
{
    if (m_dualView == dual) return;
    m_dualView = dual;
    refresh();
}

void ScopeController::setZoom(int z)
{
    z = z >= 4 ? 4 : (z >= 2 ? 2 : 1);
    if (z == m_zoom) return;
    m_zoom = z;
    QSettings().setValue(QStringLiteral("scope/zoom"), z);
    emit optionsChanged();
    refresh();
}

void ScopeController::setColorize(bool on)
{
    if (on == m_colorize) return;
    m_colorize = on;
    QSettings().setValue(QStringLiteral("scope/colorize"), on);
    emit optionsChanged();
    refresh();
}

void ScopeController::setBrightness(double b)
{
    b = std::clamp(b, 0.1, 10.0);
    if (std::abs(b - m_brightness) < 1e-6) return;
    m_brightness = b;
    QSettings().setValue(QStringLiteral("scope/brightness"), b);
    emit optionsChanged();
    refresh();
}

void ScopeController::setScaleMode(int mode)
{
    mode = std::clamp(mode, 0, 2);
    if (mode == m_scaleMode) return;
    m_scaleMode = mode;
    QSettings().setValue(QStringLiteral("scope/scaleMode"), m_scaleMode);
    emit optionsChanged();
    refresh();   // the scale changes the bins and the peaks' units
}

void ScopeController::setWaveformPeak(int nits)
{
    nits = snapPeak(nits);
    if (nits == m_wavePeak) return;
    m_wavePeak = nits;
    QSettings().setValue(QStringLiteral("scope/waveformPeak"), nits);
    emit optionsChanged();
    refresh();
}

QString ScopeController::mediaItemIdA() const
{
    if (m_mediaItemIdFn) return m_mediaItemIdFn();
    return m_project ? m_project->activeItemId() : QString();
}

void ScopeController::resetClipPeaks()
{
    // Images already measured (or in flight) belong to before the reset.
    ++m_peakEpoch;
    m_waveConfig.peakEpoch = m_peakEpoch;
    if (IPlayerRenderer *r = m_renderer ? m_renderer() : nullptr) r->setScopeConfig(m_waveConfig);
    m_clipSides = 0;
    m_clipLevel[0] = m_clipLevel[1] = 0.0f;
    m_clipChannel[0] = m_clipChannel[1] = 0.0f;
    if (!m_wavePeaks.isEmpty()) {
        m_wavePeaks.clear();
        emit peaksChanged();
    }
}

namespace {

// A peak for the readout: nits on the HDR scale (one decimal below 10),
// else a percentage of the encoded range (Y′ / code value).
QString formatPeak(float v, bool hdr)
{
    if (hdr) return v < 10.0f ? QString::number(v, 'f', 1) : QLocale().toString(qRound(v));
    return QStringLiteral("%1 %").arg(qRound(v * 100.0f));
}

} // namespace

void ScopeController::updatePeaks(const ScopePeaks &p)
{
    if (p.sides <= 0) return;
    // Measured before the last reset, or — after a clip change — of a
    // frame the new clip hasn't replaced yet (the old one stays on screen
    // until the new clip decodes).
    if (p.epoch != m_peakEpoch || p.frameStamp <= m_peakMinStamp) {
        if (qEnvironmentVariableIsSet("QCV_PEAK_DEBUG"))
            qInfo("PEAKDBG reject epoch %u/%u stamp %llu min %llu", p.epoch, m_peakEpoch,
                  (unsigned long long)p.frameStamp, (unsigned long long)m_peakMinStamp);
        return;
    }
    // A changed layout (single / dual) or scale starts over. A side in a
    // timeline gap keeps its Max and shows no frame value.
    if (m_clipSides != p.sides || m_clipHdr != p.hdr) {
        m_clipSides = p.sides;
        m_clipHdr = p.hdr;
        m_clipLevel[0] = m_clipLevel[1] = 0.0f;
        m_clipChannel[0] = m_clipChannel[1] = 0.0f;
    }
    QVariantList list;
    const QString none = QStringLiteral("—");
    for (int s = 0; s < p.sides; ++s) {
        if (p.measured[s]) {
            m_clipLevel[s]   = std::max(m_clipLevel[s], p.level[s]);
            m_clipChannel[s] = std::max(m_clipChannel[s], p.channel[s]);
        }
        QVariantMap m;
        m[QStringLiteral("side")]  = p.sides == 2 ? QString(QLatin1Char(s == 0 ? 'A' : 'B')) : QString();
        m[QStringLiteral("frame")] = p.measured[s] ? formatPeak(p.level[s], p.hdr) : none;
        m[QStringLiteral("clip")]  = formatPeak(m_clipLevel[s], p.hdr);
        const QString frameChannel = p.measured[s] ? formatPeak(p.channel[s], p.hdr) : none;
        m[QStringLiteral("tooltip")] =
            p.hdr ? tr("Brightest channel (linear Rec.2020): frame %1 · max %2 nits")
                        .arg(frameChannel, formatPeak(m_clipChannel[s], true))
                  : tr("Brightest channel (code value): frame %1 · max %2")
                        .arg(frameChannel, formatPeak(m_clipChannel[s], false));
        list.push_back(m);
    }
    if (list != m_wavePeaks) {
        m_wavePeaks = list;
        // Dev aid (with the waveform dump): log the readout as it changes.
        if (qEnvironmentVariableIsSet("QCV_WAVE_DUMP")) {
            for (const QVariant &v : m_wavePeaks) {
                const QVariantMap m = v.toMap();
                qInfo("ScopeController: peak %s frame %s max %s — %s",
                      qPrintable(m.value(QStringLiteral("side")).toString()),
                      qPrintable(m.value(QStringLiteral("frame")).toString()),
                      qPrintable(m.value(QStringLiteral("clip")).toString()),
                      qPrintable(m.value(QStringLiteral("tooltip")).toString()));
            }
        }
        emit peaksChanged();
    }
}

void ScopeController::refresh()
{
    resolve();
    buildGeometry();
    // Clip peaks belong to one clip (A / B) under one interpretation.
    // Single view, a new A also waits for its first frame — the old one
    // stays on screen until the new file decodes. This runs on
    // activeItemIdChanged, before the load opens the file
    // (ProjectManager::setActiveItem emits loadRequested after), so the
    // displayed-frame count can't include a frame of the new clip yet.
    // Dual changes (entering, B, a rebuilt island) arrive as several
    // signals with no such ordering, so there only the reset epoch
    // guards against in-flight images — as for a playlist's clip, which
    // resolves after its load.
    const QString activeId = m_project ? m_project->activeItemId() : QString();
    if (activeId != m_gateItemA) {
        m_gateItemA = activeId;
        if (!m_dualView) {
            IPlayerRenderer *r = m_renderer ? m_renderer() : nullptr;
            m_peakMinStamp = r ? r->displayedFrameCount() : 0;
            if (qEnvironmentVariableIsSet("QCV_PEAK_DEBUG"))
                qInfo("PEAKDBG clip change min %llu", (unsigned long long)m_peakMinStamp);
        }
    }
    const QString clipKey = mediaItemIdA()
        + QLatin1Char('|') + (m_dualView && m_project ? m_project->bSourceMediaId() : QString())
        + QLatin1Char('|') + QString::number(m_dualView ? 2 : 1);
    const QString peakKey = clipKey
        + QLatin1Char('|') + QString::number(static_cast<int>(m_waveConfig.tier))
        + QLatin1Char('|') + m_waveConfig.colorspace + QLatin1Char('|') + m_waveConfig.configPath
        + QLatin1Char('|') + QString::number(static_cast<int>(m_waveConfig.tierB))
        + QLatin1Char('|') + m_waveConfig.colorspaceB + QLatin1Char('|') + m_waveConfig.configPathB
        + QLatin1Char('|') + QString::number(static_cast<int>(m_waveConfig.scale));
    if (peakKey != m_peakKey) {
        m_peakKey = peakKey;
        resetClipPeaks();
    }
    push();
    emit stateChanged();
}

void ScopeController::resolve()
{
    m_config.active      = m_active;
    m_config.zoom        = m_zoom;
    m_config.colorize    = m_colorize;
    m_config.brightness  = static_cast<float>(m_brightness);
    m_config.dual        = m_dualView;
    m_config.tier        = ScopeTier::Signal;
    m_config.colorspace.clear();
    m_config.scale       = ScopeScale::Sdr;
    m_mismatch.clear();

    OCIO::ConstConfigRcPtr cfg;
    if (m_ocio) {
        try {
            cfg = OCIO::Config::CreateFromFile(m_ocio->configIdentifier().toUtf8().constData());
        } catch (const OCIO::Exception &) {}
    }

    // What a file says: its assumed colourspace (tags / format rules) in
    // config `cfgX`, why, and the Signal-tier matrix / curve.
    // CONFIG UPGRADE: the names live in color/scope_names.h, checked by
    // tools/probe-ocio-metal against the bundled configs.
    struct Reading {
        QString assumed, why;
        bool    tagged = false;
        bool    set = false;      // the user's transfer override, not a tag
        bool    still = false, exr = false;
        int     matrix = 1;
    };
    auto read = [&](const QVariantMap &item, const OCIO::ConstConfigRcPtr &cfgX) {
        Reading r;
        const QVariantMap video = item.value(QStringLiteral("video")).toMap();
        const int type = item.value(QStringLiteral("type"), -1).toInt();
        const QString ext = QFileInfo(item.value(QStringLiteral("path")).toString()).suffix().toLower();
        const QString transfer  = video.value(QStringLiteral("colorTransfer")).toString().toLower();
        const QString primaries = video.value(QStringLiteral("colorPrimaries")).toString().toLower();
        const QString space     = video.value(QStringLiteral("colorspace")).toString().toLower();
        r.still = type == 2 || type == 3;   // Image, ImageSequence
        r.exr   = ext == QLatin1String("exr");
        r.matrix = space.contains(QLatin1String("2020")) ? 2
            : (space.contains(QLatin1String("170m")) || space.contains(QLatin1String("470bg"))
               || space.contains(QLatin1String("601"))) ? 0 : 1;
        r.tagged = !transfer.isEmpty() && transfer != QLatin1String("unknown")
                   && transfer != QLatin1String("unspecified");
        // The clip's transfer override (Inspector / live strip pill) beats
        // tags and format rules: it exists for sources whose tags are
        // missing or wrong — untagged PQ exports, SRT streams, a QCBridge
        // Transmit feed. Reads as "set" so the Input mismatch note can
        // word it as the user's call rather than the file's.
        const int override_ = item.value(QStringLiteral("transferOverride"), 0).toInt();
        if (override_ > 0) {
            switch (override_) {
            case 2:  r.assumed = firstKnown(cfgX, scope_names::kPq2020);    break;
            case 3:  r.assumed = firstKnown(cfgX, scope_names::kPqP3);      break;
            case 4:  r.assumed = firstKnown(cfgX, scope_names::kHlg);       break;
            case 5:  r.assumed = firstKnown(cfgX, scope_names::kExrLinear); break;
            default: r.assumed = firstKnown(cfgX, scope_names::kSdrVideo);  break;
            }
            r.tagged = true;
            r.set = true;
            r.why = tr("set");
            r.assumed = canonicalName(cfgX, r.assumed);
            return r;
        }
        if (r.still) {
            if (r.exr) {
                r.assumed = firstKnown(cfgX, scope_names::kExrLinear);
                r.why = tr("EXR default");
            } else {
                r.assumed = firstKnown(cfgX, scope_names::kStill);
                r.why = tr("still");
            }
        } else if (type == 0 || type == 6) {           // Video, LiveStream
            if (transfer == QLatin1String("smpte2084")) {
                if (primaries.contains(QLatin1String("432")) || primaries.contains(QLatin1String("431"))
                    || primaries.contains(QLatin1String("p3"))) {
                    r.assumed = firstKnown(cfgX, scope_names::kPqP3);
                } else {
                    r.assumed = firstKnown(cfgX, scope_names::kPq2020);
                }
            } else if (transfer.contains(QLatin1String("arib")) || transfer.contains(QLatin1String("hlg"))) {
                r.assumed = firstKnown(cfgX, scope_names::kHlg);
            } else {
                r.assumed = firstKnown(cfgX, scope_names::kSdrVideo);
            }
            r.why = r.tagged ? tr("tags") : tr("untagged");
        }
        // Aliases (e.g. Blender's "Rec.1886" answers to "Rec.1886 Rec.709 -
        // Display") resolve to the config's own name for the badge.
        r.assumed = canonicalName(cfgX, r.assumed);
        return r;
    };

    // A colourspace the scope can convert: known, not data, and the config
    // has the interchange role on its side (what buildScope needs — else
    // the renderer would drop to Signal under an Assumed / Input badge).
    auto usable = [&](const OCIO::ConstConfigRcPtr &cfgX, const QString &cs) {
        if (!cfgX || cs.isEmpty()) return false;
        OCIO::ConstColorSpaceRcPtr c = cfgX->getColorSpace(cs.toUtf8().constData());
        if (!c || c->isData()) return false;
        const bool scene = c->getReferenceSpaceType() == OCIO::REFERENCE_SPACE_SCENE;
        return cfgX->hasRole(scene ? "aces_interchange" : "cie_xyz_d65_interchange");
    };
    // SDR scale for SDR-encoded video, HDR for everything else. Configs
    // (Blender's Rec.1886 among them) don't always set `encoding`; then
    // the name decides, like OcioChainBuilder's display-output rule.
    // CONFIG UPGRADE: the name fallback matches substrings of colourspace
    // names; recheck it against new configs' names.
    auto scaleFor = [&](const OCIO::ConstConfigRcPtr &cfgX, const QString &cs) {
        OCIO::ConstColorSpaceRcPtr c = cfgX->getColorSpace(cs.toUtf8().constData());
        const QByteArray enc = c ? QByteArray(c->getEncoding()) : QByteArray();
        if (!enc.isEmpty()) return enc == "sdr-video" ? ScopeScale::Sdr : ScopeScale::Hdr;
        const QString n = c ? QString::fromUtf8(c->getName()) : cs;
        for (const char *hdr : {"PQ", "HLG", "2100", "ST2084", "EDR", "Linear", "Log",
                                "ACES", "scene"}) {
            if (n.contains(QLatin1String(hdr), Qt::CaseInsensitive)) return ScopeScale::Hdr;
        }
        return ScopeScale::Sdr;
    };

    // Nits per 1.0 of a side's linear value on the HDR scale: a
    // display-referred HDR decode (PQ / HLG via the display interchange)
    // is absolute, 1.0 = 100 nits by OCIO's convention; an SDR-encoded or
    // scene-referred source's 1.0 is reference white, 203 nits (BT.2408 —
    // the amber line), chris's call 2026-10-06 over the earlier 100.
    auto whiteFor = [&](const OCIO::ConstConfigRcPtr &cfgX, const QString &cs, bool hdr) {
        if (!hdr) return 203.0f;
        OCIO::ConstColorSpaceRcPtr c = cfgX ? cfgX->getColorSpace(cs.toUtf8().constData())
                                            : OCIO::ConstColorSpaceRcPtr();
        const bool scene = c && c->getReferenceSpaceType() == OCIO::REFERENCE_SPACE_SCENE;
        return scene ? 203.0f : 100.0f;
    };

    // The built-in fallback config, for files the live config can't name
    // (loaded only when it is a different file).
    const QString fallbackPath =
        OCIOConfigManager::bundledConfigPath(QString::fromLatin1(kScopeFallbackConfigDir));
    OCIO::ConstConfigRcPtr fallbackCfg;
    if (!fallbackPath.isEmpty()
        && (!m_ocio || QFileInfo(fallbackPath).canonicalFilePath()
                           != QFileInfo(m_ocio->configIdentifier()).canonicalFilePath())) {
        try {
            fallbackCfg = OCIO::Config::CreateFromFile(fallbackPath.toUtf8().constData());
        } catch (const OCIO::Exception &) {}
    }

    // One side's tier, colourspace and badge. OCIO engaged: that side's
    // Input — the clip's pin, else the default —
    // so the scopes show what the picture shows. OCIO off: each file's
    // own assumption, from the live config, else the built-in one
    // (labelled).
    struct Side {
        ScopeTier tier = ScopeTier::Signal;
        QString   colorspace, configPath, badge, mismatch;
        bool      hdr = false;
        float     white = 100.0f;   // nits per 1.0 linear (whiteFor)
        Reading   r;
    };
    const auto chains = m_ocio ? m_ocio->snapshot() : nullptr;
    const bool engaged = chains && chains->engaged;
    auto interpret = [&](const QVariantMap &item, const QString &input) {
        Side sd;
        sd.r = read(item, cfg);
        const Reading rf = fallbackCfg ? read(item, fallbackCfg) : Reading{};
        const bool fallbackNames = !usable(cfg, sd.r.assumed) && usable(fallbackCfg, rf.assumed);
        if (engaged && usable(cfg, input)) {
            sd.tier = ScopeTier::Input;
            sd.colorspace = input;
            sd.hdr = scaleFor(cfg, input) == ScopeScale::Hdr;
            sd.white = whiteFor(cfg, input, sd.hdr);
            sd.badge = tr("Input · %1").arg(input);
            if (sd.r.tagged && !sd.r.assumed.isEmpty()
                && canonicalName(cfg, sd.r.assumed) != canonicalName(cfg, input)) {
                sd.mismatch = (sd.r.set ? tr("⚠ Set to %1 · Input: %2")
                                        : tr("⚠ File tagged %1 · Input: %2"))
                                  .arg(sd.r.assumed, input);
            } else if (sd.r.tagged && fallbackNames) {
                sd.mismatch = (sd.r.set ? tr("⚠ Set to %1 — not in this config · Input: %2")
                                        : tr("⚠ File tagged %1 — not in this config · Input: %2"))
                                  .arg(rf.assumed, input);
            }
        } else if (usable(cfg, sd.r.assumed)) {
            sd.tier = ScopeTier::Assumed;
            sd.colorspace = sd.r.assumed;
            sd.hdr = scaleFor(cfg, sd.colorspace) == ScopeScale::Hdr;
            sd.white = whiteFor(cfg, sd.colorspace, sd.hdr);
            sd.badge = tr("Assumed · %1 (%2)").arg(sd.r.assumed, sd.r.why);
        } else if (fallbackNames) {
            sd.tier = ScopeTier::Assumed;
            sd.colorspace = rf.assumed;
            sd.configPath = fallbackPath;
            sd.hdr = scaleFor(fallbackCfg, sd.colorspace) == ScopeScale::Hdr;
            sd.white = whiteFor(fallbackCfg, sd.colorspace, sd.hdr);
            sd.badge = tr("Assumed · %1 (%2, built-in config)").arg(rf.assumed, rf.why);
        } else {
            sd.badge = sd.r.exr || sd.r.still ? tr("Signal · RGB, nominal curve")
                                              : tr("Signal · YUV");
        }
        return sd;
    };

    const QString inputA = !chains ? QString()
                         : (m_dualView ? chains->a.scene.input : chains->single.scene.input);
    const Side a = interpret(m_project ? m_project->mediaItemMap(mediaItemIdA()) : QVariantMap{},
                             inputA);
    m_config.tier = a.tier;
    m_config.colorspace = a.colorspace;
    m_config.configPath = a.configPath;
    m_config.signalMatrix = a.r.matrix;
    m_config.signalNominalCurve = a.r.exr;
    m_config.whiteNits = a.white;
    m_badge = a.badge;
    m_mismatch = a.mismatch;
    m_config.tierB = a.tier;
    m_config.colorspaceB = a.colorspace;
    m_config.configPathB = a.configPath;
    bool bHdr = a.hdr;
    m_config.signalMatrixB = a.r.matrix;
    m_config.signalNominalCurveB = a.r.exr;
    m_config.whiteNitsB = a.white;
    if (m_dualView && m_project) {
        const QVariantMap itemB = m_project->bSourceItemMap();
        if (!itemB.isEmpty()) {
            const Side b = interpret(itemB, chains ? chains->b.scene.input : QString());
            m_config.tierB = b.tier;
            m_config.colorspaceB = b.colorspace;
            m_config.configPathB = b.configPath;
            bHdr = b.hdr;
            m_config.signalMatrixB = b.r.matrix;
            m_config.signalNominalCurveB = b.r.exr;
            m_config.whiteNitsB = b.white;
            if (b.badge != a.badge) m_badge = tr("A %1  ·  B %2").arg(a.badge, b.badge);
            if (m_mismatch.isEmpty()) m_mismatch = b.mismatch;
        }
    }
    // The scale is shared: HDR when either side needs it (an SDR side
    // then plots in nits too, its white at 100).
    m_hdrScale = a.hdr || (m_dualView && bHdr);

    // Signal draws mono: colours would claim to know the colour space.
    m_config.colorize = m_colorize && m_config.tier != ScopeTier::Signal;
    m_config.kind = ScopeKind::Vectorscope;
    if (m_config.tier != ScopeTier::Signal || m_config.tierB != ScopeTier::Signal) {
        // The manual scale (panel chips) overrides the interpretation's
        // choice. Forcing SDR on a PQ side plots that side's own Y′; forcing
        // nits on an SDR side converts it, white at 203.
        bool hdr = m_hdrScale;
        if (m_scaleMode == 1)      hdr = false;
        else if (m_scaleMode == 2) hdr = true;
        m_config.scale = hdr ? ScopeScale::Hdr : ScopeScale::Sdr;
        m_scaleLabel = m_config.scale == ScopeScale::Sdr ? tr("SDR · Rec.709")
                                                         : tr("HDR · PQ Rec.2020");
        if (m_scaleMode != 0 && hdr != m_hdrScale) m_scaleLabel += tr(" (manual)");
    } else {
        static const char *kMatrix[] = {"BT.601", "BT.709", "BT.2020"};
        m_scaleLabel = tr("%1 matrix").arg(QString::fromLatin1(kMatrix[m_config.signalMatrix]));
    }
}

void ScopeController::buildGeometry()
{
    // Waveform config: the vectorscope's interpretation, binned by column
    // and level; mono.
    m_waveConfig = m_config;
    m_waveConfig.kind     = ScopeKind::Waveform;
    m_waveConfig.active   = m_waveActive;
    m_waveConfig.colorize = false;
    m_waveConfig.zoom     = 1;
    m_waveConfig.waveformPeakNits = m_wavePeak;
    // SDR: the file's own Y′ — what a broadcast waveform shows, and the only
    // way to keep sub-blacks, which some configs' SDR decodes clamp
    // (Blender's Rec.1886). HDR / linear / log: the converted PQ scale.
    // Dual: an SDR side beside an HDR one converts too, into nits.
    const bool hdr = m_config.scale == ScopeScale::Hdr;
    m_waveHdr = hdr;
    static const char *kMatrix[] = {"BT.601", "BT.709", "BT.2020"};
    if (!hdr) {
        m_waveConfig.tier = ScopeTier::Signal;
        m_waveConfig.colorspace.clear();
        m_waveConfig.tierB = ScopeTier::Signal;
        m_waveConfig.colorspaceB.clear();
        m_waveBadge = tr("Signal Y′ · %1 matrix")
                          .arg(QString::fromLatin1(kMatrix[m_config.signalMatrix]));
    } else {
        m_waveBadge = m_badge + tr(" · luminance");
    }

    // Level lines.
    m_waveLines.clear();
    float lo, hi;
    scope_math::waveformRange(hdr, static_cast<float>(m_wavePeak), lo, hi);
    auto addLine = [&](float v, const QString &label, bool major, bool ref = false) {
        QVariantMap m;
        m[QStringLiteral("y")]     = (hi - v) / (hi - lo);
        m[QStringLiteral("label")] = label;
        m[QStringLiteral("major")] = major;
        m[QStringLiteral("ref")]   = ref;
        m_waveLines.push_back(m);
    };
    if (hdr) {
        // Linear nits: round steps to the peak, every other one major, and
        // HDR reference white (203, BT.2408) as its own line — a step
        // closer than 4 % of the scale gives way to it.
        const int step = m_wavePeak <= 300 ? 50 : (m_wavePeak <= 1000 ? 100
                                                  : (m_wavePeak <= 2000 ? 200 : 500));
        auto label = [](int n) {
            return n >= 1000 && n % 1000 == 0 ? QStringLiteral("%1k").arg(n / 1000)
                                              : QString::number(n);
        };
        constexpr int kRefWhite = 203;
        addLine(0.0f, QStringLiteral("0"), true);
        for (int n = step; n <= m_wavePeak; n += step) {
            if (std::abs(n - kRefWhite) < 0.04f * m_wavePeak) continue;
            addLine(float(n), label(n), n % (2 * step) == 0);
        }
        addLine(float(kRefWhite), QStringLiteral("203"), false, true);
    } else {
        for (int pct = 0; pct <= 100; pct += 25) {
            addLine(pct / 100.0f, QString::number(pct), pct == 0 || pct == 100);
        }
    }

    m_targets.clear();
    m_hexagons.clear();
    m_skinLine.clear();
    if (m_config.tier == ScopeTier::Signal) return;

    const ScopeScale scale = m_config.scale;
    // On the HDR scale 1.0 of these targets is reference white, 203 nits
    // (where an SDR / scene-referred side's white lands — whiteFor).
    constexpr float kTargetWhite = 203.0f;
    // Colour-bar targets in the scale's own primaries at 100 % (1.0 =
    // SDR white) and 75 % (encoded 0.75 → 0.75^2.4 linear).
    struct Bar { const char *label; float r, g, b; };
    static const Bar kBars[] = {{"R", 1, 0, 0}, {"Yl", 1, 1, 0}, {"G", 0, 1, 0},
                                {"Cy", 0, 1, 1}, {"B", 0, 0, 1}, {"Mg", 1, 0, 1}};
    const float lin75 = std::pow(0.75f, 2.4f);
    for (float level : {1.0f, lin75}) {
        for (const Bar &bar : kBars) {
            float c[3] = {bar.r * level, bar.g * level, bar.b * level}, l[3];
            if (scale == ScopeScale::Sdr) rec709To2020(c, l);
            else { l[0] = c[0]; l[1] = c[1]; l[2] = c[2]; }
            float x, y;
            scope_math::targetPoint(l, scale, m_zoom, x, y, kTargetWhite);
            QVariantMap t = point(x, y);
            t[QStringLiteral("label")] = QString::fromLatin1(bar.label);
            t[QStringLiteral("full")]  = level == 1.0f;
            m_targets.push_back(t);
        }
    }

    // Gamut hexagons (Input tier): Rec.709, P3-D65, Rec.2020 primaries +
    // secondaries at SDR white. Out-of-gamut colours land outside.
    if (m_config.tier == ScopeTier::Input) {
        struct Gamut { const char *name; int kind; };
        for (const Gamut &g : {Gamut{"709", 0}, Gamut{"P3", 1}, Gamut{"2020", 2}}) {
            QVariantList pts;
            for (const Bar &bar : kBars) {
                float c[3] = {bar.r, bar.g, bar.b}, l[3];
                if (g.kind == 0)      rec709To2020(c, l);
                else if (g.kind == 1) p3To2020(c, l);
                else { l[0] = c[0]; l[1] = c[1]; l[2] = c[2]; }
                float x, y;
                scope_math::targetPoint(l, scale, m_zoom, x, y, kTargetWhite);
                pts << x << y;
            }
            QVariantMap h;
            h[QStringLiteral("name")]   = QString::fromLatin1(g.name);
            h[QStringLiteral("points")] = pts;
            m_hexagons.push_back(h);
        }
    }

    // Skin-tone line: the NTSC I-axis, 123° from +Cb.
    const double a = 123.0 * 3.14159265358979323846 / 180.0;
    m_skinLine << 0.5 + 0.5 * std::cos(a) << 0.5 - 0.5 * std::sin(a);
}

void ScopeController::push()
{
    m_waveConfig.peakEpoch = m_peakEpoch;
    if (IPlayerRenderer *r = m_renderer ? m_renderer() : nullptr) {
        r->setScopeConfig(m_config);
        r->setScopeConfig(m_waveConfig);
    }
}

void ScopeController::poll()
{
    IPlayerRenderer *r = m_renderer ? m_renderer() : nullptr;
    if (!r) return;
    if (m_waveActive && m_waveProvider) {
        QImage wimg;
        quint64 wserial = 0;
        ScopePeaks peaks;
        if (r->scopeImage(&wimg, &wserial, ScopeKind::Waveform, &peaks)
            && wserial != m_lastWaveSerial) {
            m_lastWaveSerial = wserial;
            updatePeaks(peaks);
            m_waveProvider->setImage(wimg);
            m_waveImageSource = QStringLiteral("image://qcvwave/%1").arg(wserial);
            static const QByteArray wdump = qgetenv("QCV_WAVE_DUMP");
            if (!wdump.isEmpty() && wserial % 30 == 1) {
                QImage onBlack(wimg.size(), QImage::Format_RGB32);
                onBlack.fill(Qt::black);
                QPainter(&onBlack).drawImage(0, 0, wimg);
                onBlack.save(QString::fromLocal8Bit(wdump));
            }
            emit imageChanged();
        }
    }
    if (!m_active || !m_provider) return;
    QImage img;
    quint64 serial = 0;
    if (!r->scopeImage(&img, &serial) || serial == m_lastSerial) return;
    m_lastSerial = serial;
    m_provider->setImage(img);
    // Dev aid: QCV_SCOPE_DUMP=<file.png> saves the trace (every ~30th
    // image) for checking the scope without a screen capture.
    static const QByteArray dump = qgetenv("QCV_SCOPE_DUMP");
    if (!dump.isEmpty() && serial % 30 == 1) {
        QImage onBlack(img.size(), QImage::Format_RGB32);
        onBlack.fill(Qt::black);
        QPainter(&onBlack).drawImage(0, 0, img);
        onBlack.save(QString::fromLocal8Bit(dump));
    }
    m_imageSource = QStringLiteral("image://qcvscope/%1").arg(serial);
    emit imageChanged();
}

} // namespace qcv
