#include "ocio_config_manager.h"

#include <algorithm>
#include <cmath>

#include "colourspace_short_name.h"
#include "ocio_chain_builder.h"
#include "ocio_lut_baker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QtLogging>

#include <cstdlib>

#include <OpenColorIO/OpenColorIO.h>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

namespace {

// Phase 2.5c: friendly names for shipped config directories.
// Anything not in the table falls back to the dir name as-is.
// CONFIG UPGRADE: a new Blender / ACES directory needs a row here, the
// default promotion below, and ScopeController's kScopeFallbackConfigDir
// if the scopes' fallback moves — checklist in
// assets/OCIO/patches/README.md ("Names QCView depends on").
QString friendlyNameForDir(const QString &dirName)
{
    static const QHash<QString, QString> table{
        { QStringLiteral("Blender5.2"), QStringLiteral("Blender 5.2") },
        { QStringLiteral("Blender5.1"), QStringLiteral("Blender 5.1") },
        { QStringLiteral("Blender"),    QStringLiteral("Blender (legacy)") },
        { QStringLiteral("ACES_2.0"),   QStringLiteral("ACES 2.0") },
        { QStringLiteral("ACES_1.3"),   QStringLiteral("ACES 1.3") },
    };
    return table.value(dirName, dirName);
}

// Version gate per Guide 05 D10. A config that declares a higher
// OCIO profile than the linked library can't be safely loaded —
// reject before we let OCIO throw mid-build.
bool isVersionAcceptable(const OCIO::ConstConfigRcPtr &cfg, QString *outReason)
{
    if (!cfg) {
        if (outReason) *outReason = QStringLiteral("null config");
        return false;
    }
    const int cfgMajor = cfg->getMajorVersion();
    const int cfgMinor = cfg->getMinorVersion();
    const int libMajor = OCIO::GetVersionHex() >> 24;
    const int libMinor = (OCIO::GetVersionHex() >> 16) & 0xFF;
    if (cfgMajor > libMajor
        || (cfgMajor == libMajor && cfgMinor > libMinor)) {
        if (outReason) {
            *outReason = QStringLiteral(
                "config declares OCIO %1.%2; linked library supports up to %3.%4")
                .arg(cfgMajor).arg(cfgMinor).arg(libMajor).arg(libMinor);
        }
        return false;
    }
    return true;
}

// Phase 2.5: LUT slot extensions OCIO's FileTransform can load.
// .cube is by far the most common interchange format; .3dl is older
// (Discreet) but still used by some grading workflows; .csp is OCIO's
// shaper-paired format.
bool isSupportedLutExtension(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("cube")
        || suffix == QStringLiteral("3dl")
        || suffix == QStringLiteral("csp");
}

// ASC CDL — the grade a dailies colourist sends (.cc single correction,
// .ccc / .cdl collections). Scene LUT slot only; OCIO's FileTransform
// reads them natively, the first correction unless a correction ID is set.
bool isCdlExtension(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("cc")
        || suffix == QStringLiteral("ccc")
        || suffix == QStringLiteral("cdl");
}

// Resolve the bundled OCIO assets directory.
//
// macOS .app bundle:  applicationDirPath() = qcview.app/Contents/MacOS
//                     assets at:            qcview.app/Contents/Resources/assets/OCIO
//                     → ../Resources/assets/OCIO
//
// Dev build (cmake)    is the same — `target_sources` with
//                      MACOSX_PACKAGE_LOCATION puts the assets in the
//                      built .app's Contents/Resources, so the same
//                      relative-path resolution works whether the
//                      .app is run from the build directory or from
//                      a deployed bundle.
//
// Returns an empty QString if the directory doesn't exist (caller
// falls back to ocio://default with a warning).
QString resolveOcioAssetsDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/../Resources/assets/OCIO"),    // macOS bundle
        appDir + QStringLiteral("/assets/OCIO"),                  // future Linux/Win
        appDir + QStringLiteral("/../../assets/OCIO"),            // out-of-tree build
    };
    for (const QString &path : candidates) {
        const QString canonical = QDir(path).canonicalPath();
        if (!canonical.isEmpty() && QFileInfo(canonical).isDir()) {
            return canonical;
        }
    }
    return {};
}

} // namespace

struct OCIOConfigManager::Impl {
    OCIO::ConstConfigRcPtr config;
};

QString OCIOConfigManager::bundledConfigPath(const QString &dirName)
{
    const QString dir = resolveOcioAssetsDir();
    if (dir.isEmpty()) return {};
    const QString path = dir + QLatin1Char('/') + dirName + QStringLiteral("/config.ocio");
    return QFileInfo::exists(path) ? path : QString();
}

OCIOConfigManager::OCIOConfigManager(QObject *parent)
    : QObject(parent), m_impl(std::make_unique<Impl>())
{
    // Phase 2.5c: build the enumerated config slot list once at
    // startup ($OCIO env + scan of bundled assets/OCIO/), then load
    // the highest-priority entry per Guide 05 §3:
    //   1. Explicit user choice in Settings  (not yet — Phase 7+)
    //   2. $OCIO env var (if set + valid + passes version gate)
    //   3. Shipped default (Blender 5.2)
    //   4. OCIO library built-in (last-resort)
    enumerateConfigs();

    // The knee's read-outs (target, start in nits) depend on the active
    // Display/View; refresh them on chain changes. Signal only — the
    // renderers poll stageGeneration(), which this does not bump.
    connect(this, &OCIOConfigManager::activeChainChanged,
            this, &OCIOConfigManager::kneeChanged);
    connect(this, &OCIOConfigManager::configChanged,
            this, &OCIOConfigManager::kneeChanged);
    // Badge short names follow the config.
    connect(this, &OCIOConfigManager::configChanged,
            this, &OCIOConfigManager::bumpPinsRevision);

    bool loaded = false;
    for (const ConfigSlot &slot : m_configSlots) {
        qInfo("OCIOConfigManager: loading '%s' from %s",
              qPrintable(slot.displayName), qPrintable(slot.path));
        if (loadConfigFile(slot.path)) {
            m_activeConfigName = slot.displayName;
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        qWarning("OCIOConfigManager: no enumerated config loaded; "
                 "falling back to ocio://default");
        if (loadBuiltInDefault()) {
            m_activeConfigName = QStringLiteral("OCIO built-in default");
            // Add the built-in to the list so the UI has something
            // to display in the Config reel.
            m_configSlots.prepend({ m_activeConfigName,
                                    QStringLiteral("ocio://default") });
            emit availableConfigsChanged();
        }
    }
}

void OCIOConfigManager::enumerateConfigs()
{
    m_configSlots.clear();

    // $OCIO env var — highest priority of the auto-discovered slots.
    if (const char *envOcio = std::getenv("OCIO"); envOcio && *envOcio) {
        const QString envPath = QString::fromUtf8(envOcio);
        if (QFileInfo::exists(envPath)) {
            // Version gate the env config before listing it. If it's
            // out-of-version we still list it, but loadConfigFile
            // will reject it on attempt — and the next slot wins.
            m_configSlots.append({ QStringLiteral("$OCIO"), envPath });
            qInfo("OCIOConfigManager: $OCIO points to %s", qPrintable(envPath));
        } else {
            qWarning("OCIOConfigManager: $OCIO=%s — file does not exist; ignored",
                     envOcio);
        }
    }

    // Bundled configs — scan assets/OCIO/* for any subdir containing
    // a config.ocio. Order is alphabetical by friendly name except
    // Blender 5.2 is always first (default config; 5.1 kept for
    // user presets that reference it by name).
    const QString assetsDir = resolveOcioAssetsDir();
    if (!assetsDir.isEmpty()) {
        QDir d(assetsDir);
        const QFileInfoList subdirs =
            d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        QList<ConfigSlot> bundled;
        for (const QFileInfo &sub : subdirs) {
            const QString cfg = sub.absoluteFilePath()
                                + QStringLiteral("/config.ocio");
            if (QFileInfo::exists(cfg)) {
                bundled.append({ friendlyNameForDir(sub.fileName()), cfg });
            }
        }
        // Promote Blender 5.2 to the front of the bundled list.
        // CONFIG UPGRADE: the default config — moves with the scopes'
        // fallback (ScopeController kScopeFallbackConfigDir).
        for (int i = 0; i < bundled.size(); ++i) {
            if (bundled[i].displayName == QStringLiteral("Blender 5.2")) {
                bundled.move(i, 0);
                break;
            }
        }
        m_configSlots.append(bundled);
    }

    emit availableConfigsChanged();
}

OCIOConfigManager::~OCIOConfigManager() = default;

bool OCIOConfigManager::loadBuiltInDefault()
{
    try {
        m_impl->config = OCIO::Config::CreateFromFile("ocio://default");
        m_configIdentifier = QStringLiteral("ocio://default");
    } catch (const OCIO::Exception &e) {
        qWarning("OCIOConfigManager: built-in default load failed: %s", e.what());
        return false;
    }
    resetActiveDefaults();
    emit configChanged();
    return true;
}

bool OCIOConfigManager::loadConfigFile(const QString &path)
{
    OCIO::ConstConfigRcPtr candidate;
    try {
        candidate = OCIO::Config::CreateFromFile(path.toUtf8().constData());
    } catch (const OCIO::Exception &e) {
        qWarning("OCIOConfigManager: load %s failed: %s",
                 qPrintable(path), e.what());
        return false;
    }
    QString reason;
    if (!isVersionAcceptable(candidate, &reason)) {
        qWarning("OCIOConfigManager: rejected %s — %s",
                 qPrintable(path), qPrintable(reason));
        return false;
    }
    m_impl->config = candidate;
    m_configIdentifier = path;
    resetActiveDefaults();
    emit configChanged();
    return true;
}

bool OCIOConfigManager::setActiveConfig(const QString &name)
{
    if (name == m_activeConfigName) return true;
    for (const ConfigSlot &slot : m_configSlots) {
        if (slot.displayName == name) {
            // For the built-in URI entry, route through the URI loader.
            const bool ok = (slot.path == QStringLiteral("ocio://default"))
                ? loadBuiltInDefault()
                : loadConfigFile(slot.path);
            if (ok) {
                m_activeConfigName = name;
                emit configChanged();   // configIdentifier + activeConfigName
            }
            return ok;
        }
    }
    qWarning("OCIOConfigManager: unknown config '%s'", qPrintable(name));
    return false;
}

QStringList OCIOConfigManager::availableConfigs() const
{
    QStringList out;
    out.reserve(m_configSlots.size());
    for (const ConfigSlot &slot : m_configSlots) {
        out << slot.displayName;
    }
    return out;
}

QString OCIOConfigManager::configDescription() const
{
    if (!m_impl->config) return {};
    const char *desc = m_impl->config->getDescription();
    if (!desc) return {};
    const QString full = QString::fromUtf8(desc).trimmed();
    if (full.isEmpty()) return full;

    // The ACES configs ship with multi-line descriptions plus an
    // ASCII rule and trailing version-bracket tags, e.g.
    //   "Academy Color Encoding System - Studio Config (All Views)
    //    [COLORSPACES v4.0.0] [ACES v2.0] [OCIO v2.5]
    //    ----------------------------------------..."
    // The panel header is single-line, so collapse to the first
    // non-rule content line and strip the trailing "[...] [...]"
    // version tags.
    QString line;
    for (const QString &raw : full.split(QChar('\n'))) {
        const QString t = raw.trimmed();
        if (t.isEmpty()) continue;
        bool allDash = (t.size() >= 4);
        for (QChar c : t) {
            if (c != QLatin1Char('-')) { allDash = false; break; }
        }
        if (allDash) continue;
        line = t;
        break;
    }
    if (line.isEmpty()) return full;

    // Strip trailing "  [foo]  [bar]  ..." groups.
    while (line.endsWith(QLatin1Char(']'))) {
        const int open = line.lastIndexOf(QLatin1Char('['));
        if (open <= 0) break;
        line.truncate(open);
        line = line.trimmed();
    }
    return line;
}

QString OCIOConfigManager::configIdentifier() const
{
    return m_configIdentifier;
}

QStringList OCIOConfigManager::colorspaces() const
{
    QStringList out;
    if (!m_impl->config) return out;
    for (int i = 0; i < m_impl->config->getNumColorSpaces(); ++i) {
        out << QString::fromUtf8(m_impl->config->getColorSpaceNameByIndex(i));
    }
    return out;
}

QStringList OCIOConfigManager::displays() const
{
    QStringList out;
    if (!m_impl->config) return out;
    for (int i = 0; i < m_impl->config->getNumDisplays(); ++i) {
        out << QString::fromUtf8(m_impl->config->getDisplay(i));
    }
    return out;
}

QStringList OCIOConfigManager::looks() const
{
    QStringList out;
    if (!m_impl->config) return out;
    for (int i = 0; i < m_impl->config->getNumLooks(); ++i) {
        out << QString::fromUtf8(m_impl->config->getLookNameByIndex(i));
    }
    return out;
}

QStringList OCIOConfigManager::viewsForDisplay(const QString &display) const
{
    QStringList out;
    if (!m_impl->config || display.isEmpty()) return out;
    const QByteArray d = display.toUtf8();
    const int n = m_impl->config->getNumViews(d.constData());
    for (int i = 0; i < n; ++i) {
        out << QString::fromUtf8(m_impl->config->getView(d.constData(), i));
    }
    return out;
}

bool OCIOConfigManager::sdrCaptureDisplayView(QString *display, QString *view) const
{
    if (!m_impl->config || m_activeDisplay.isEmpty() || m_activeView.isEmpty()) {
        return false;
    }

    // Target display: "sRGB" (Blender) / "sRGB - Display" (ACES
    // studio configs), else the first sRGB-named display that isn't
    // one of our linear EDR ones.
    // CONFIG UPGRADE: recheck these display names against new configs.
    const QStringList allDisplays = displays();
    QString target;
    for (const QString &d : allDisplays) {
        if (d == QLatin1String("sRGB") || d == QLatin1String("sRGB - Display")) {
            target = d;
            break;
        }
    }
    if (target.isEmpty()) {
        for (const QString &d : allDisplays) {
            if (d.startsWith(QLatin1String("sRGB"), Qt::CaseInsensitive)
                && !d.contains(QLatin1String("EDR"), Qt::CaseInsensitive)
                && !d.contains(QLatin1String("Linear"), Qt::CaseInsensitive)) {
                target = d;
                break;
            }
        }
    }
    if (target.isEmpty() || target == m_activeDisplay) return false;

    const QStringList views = viewsForDisplay(target);
    if (views.isEmpty()) return false;

    const QString &src = m_activeView;
    const bool srcD60 = src.contains(QLatin1String("D60"));
    auto pick = [&](const QString &v) {
        *display = target;
        *view    = v;
        return true;
    };

    // 1. Same view exists on the sRGB display ("Standard", "AgX",
    //    "Un-tone-mapped", "ACES 2.0 - SDR 100 nits (Rec.709)", "Raw").
    if (views.contains(src)) return pick(src);

    // 2. Blender naming: drop the HDR/SDR tier or the no-tonemap tag
    //    ("ACES 2.0 - HDR 1000 nits" → "ACES 2.0",
    //     "Standard (No Tonemap)" → "Standard").
    QString base = src;
    base.remove(QLatin1String(" (No Tonemap)"));
    for (const char *tier : {" - HDR", " - SDR"}) {
        const int at = base.indexOf(QLatin1String(tier));
        if (at > 0) base.truncate(at);
    }
    if (views.contains(base)) return pick(base);

    // 3. ACES studio naming: "ACES 2.0 - HDR 1000 nits (P3 D65)" →
    //    an "ACES 2.0 - SDR 100 nits (...)" view, keeping the D60
    //    sim choice ("(Rec.709)" vs "(Rec.709 D60 in Rec.709 D65)").
    // 4. Otherwise any SDR view of the same family ("ACES 1.1 - HDR
    //    Video (...)" → "ACES 1.0 - SDR Video"), same D60 rule.
    const QString family = src.section(QLatin1Char(' '), 0, 0);
    QString sdrPrefix = src;
    sdrPrefix.replace(QRegularExpression(QStringLiteral("HDR \\d+ nits.*$")),
                      QStringLiteral("SDR 100 nits"));
    for (int pass = 0; pass < 2; ++pass) {
        QString fallback;
        for (const QString &v : views) {
            const bool match = (pass == 0)
                ? (sdrPrefix != src && v.startsWith(sdrPrefix))
                : (!family.isEmpty() && v.startsWith(family)
                   && v.contains(QLatin1String("SDR")));
            if (!match) continue;
            if (v.contains(QLatin1String("D60")) == srcD60) return pick(v);
            if (fallback.isEmpty()) fallback = v;
        }
        if (!fallback.isEmpty()) return pick(fallback);
    }

    // 5. The sRGB display's default view.
    const QByteArray t = target.toUtf8();
    const char *def = m_impl->config->getDefaultView(t.constData());
    qWarning("OCIOConfigManager: no SDR capture match for view '%s' — using "
             "'%s' on '%s'", qPrintable(src), def ? def : "?", t.constData());
    return pick(def && *def ? QString::fromUtf8(def) : views.first());
}

QString OCIOConfigManager::exportLut(const QString &outPath, int cubeSize)
{
    if (!m_impl->config) {
        return QStringLiteral("No OCIO config loaded");
    }
    if (outPath.isEmpty()) {
        return QStringLiteral("Output path is empty");
    }
    if (cubeSize < 2) {
        return QStringLiteral("Cube size must be ≥ 2 (got %1)").arg(cubeSize);
    }

    QString err;

    // With the Highlight Knee on, the export includes it: it is a chain
    // step, like a Look. Brightness (gain) is a viewing aid and never
    // baked, so the stage is resolved at gain 1.0.
    const OcioChainSpec spec = focusedSpec();
    const LinearStageSettings stage = spec.stage(1.0f);
    if (stage.kneeEnabled) {
        OcioSplitTransforms split;
        if (OcioChainBuilder::buildSplitTransforms(spec, m_impl->config, split, &err)) {
            try {
                OCIO::ConstCPUProcessorRcPtr pre = m_impl->config->getProcessor(split.pre)
                    ->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_DEFAULT);
                OCIO::ConstCPUProcessorRcPtr post = m_impl->config->getProcessor(split.post)
                    ->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_DEFAULT);
                const LinearStageGpu g =
                    linear_stage::resolve(stage, split.side, split.displayIsSdr);
                return OcioLutBaker::writeCube(
                    [&](float *rgb) {
                        pre->applyRGB(rgb);
                        linear_stage::apply(rgb, g);
                        post->applyRGB(rgb);
                    },
                    outPath, cubeSize);
            } catch (const OCIO::Exception &e) {
                return QStringLiteral("OCIO error: %1").arg(e.what());
            }
        }
        // Knee unavailable for this chain — export it without.
        qInfo("OCIOConfigManager: export without knee (%s)", qPrintable(err));
        err.clear();
    }

    OCIO::GroupTransformRcPtr group =
        OcioChainBuilder::buildGroupTransform(spec, m_impl->config, &err);
    if (!group) {
        return err.isEmpty() ? QStringLiteral("Chain build failed") : err;
    }

    try {
        OCIO::ConstProcessorRcPtr proc = m_impl->config->getProcessor(group);
        // OPTIMIZATION_DEFAULT enables OCIO's standard fast path
        // (LUT prebake / pixel-format short-circuit). Sub-100 ms at
        // 65³ on M-series silicon.
        OCIO::ConstCPUProcessorRcPtr cpuProc =
            proc->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_DEFAULT);
        return OcioLutBaker::writeCube(cpuProc, outPath, cubeSize);
    } catch (const OCIO::Exception &e) {
        return QStringLiteral("OCIO error: %1").arg(e.what());
    }
}

void OCIOConfigManager::setActiveInput(const QString &name)
{
    // Empty = clear (slot becomes passthrough — Guide 05 D6).
    if (!name.isEmpty() && !isValidColorSpace(name)) {
        qWarning("OCIOConfigManager: rejected unknown Input colorspace '%s'",
                 qPrintable(name));
        return;
    }
    if (OcioScenePin *pin = editPin(Slot::Input)) {
        if (pin->input && *pin->input == name) return;
        pin->input = name;
        notePinEdit(focusClipId());
    } else {
        if (m_default.input == name) return;
        m_default.input = name;
    }
    publish();
}

void OCIOConfigManager::setActiveDisplay(const QString &name)
{
    if (m_activeDisplay == name) return;
    if (!name.isEmpty() && !isValidDisplay(name)) {
        qWarning("OCIOConfigManager: rejected unknown Display '%s'",
                 qPrintable(name));
        return;
    }
    m_activeDisplay = name;
    // If current View is no longer valid for the new Display, snap
    // to the first available view (or clear if there are none — e.g.
    // when Display itself was just cleared).
    if (!m_activeDisplay.isEmpty()
        && !isValidViewForDisplay(m_activeDisplay, m_activeView)) {
        const QStringList views = viewsForDisplay(m_activeDisplay);
        m_activeView = views.isEmpty() ? QString() : views.first();
    } else if (m_activeDisplay.isEmpty()) {
        m_activeView.clear();
    }
    publish();
}

void OCIOConfigManager::setActiveView(const QString &name)
{
    if (m_activeView == name) return;
    if (!name.isEmpty()
        && !isValidViewForDisplay(m_activeDisplay, name)) {
        qWarning("OCIOConfigManager: rejected View '%s' for Display '%s'",
                 qPrintable(name), qPrintable(m_activeDisplay));
        return;
    }
    m_activeView = name;
    publish();
}

void OCIOConfigManager::setActiveLook(const QString &name)
{
    if (!name.isEmpty() && !isValidLook(name)) {
        qWarning("OCIOConfigManager: rejected unknown Look '%s'",
                 qPrintable(name));
        return;
    }
    if (OcioScenePin *pin = editPin(Slot::Look)) {
        if (pin->look && *pin->look == name) return;
        pin->look = name;
        notePinEdit(focusClipId());
    } else {
        if (m_default.look == name) return;
        m_default.look = name;
    }
    publish();
}

void OCIOConfigManager::setActiveSceneLutPath(const QString &path)
{
    if (!path.isEmpty()) {
        if (!QFileInfo::exists(path)) {
            qWarning("OCIOConfigManager: Scene LUT file not found: %s",
                     qPrintable(path));
            return;
        }
        if (!isSupportedLutExtension(path) && !isCdlExtension(path)) {
            qWarning("OCIOConfigManager: unsupported Scene LUT extension '%s' "
                     "(expected .cube / .3dl / .csp / .cc / .ccc / .cdl)",
                     qPrintable(QFileInfo(path).suffix()));
            return;
        }
    }
    // A new file starts at its first correction.
    if (OcioScenePin *pin = editPin(Slot::SceneLut)) {
        if (pin->sceneLut && pin->sceneLut->path == path) return;
        pin->sceneLut = OcioScenePin::SceneLut{path, QString()};
        notePinEdit(focusClipId());
    } else {
        if (m_default.sceneLutPath == path) return;
        m_default.sceneLutPath = path;
        m_default.sceneLutCccId.clear();
    }
    publish();
}

void OCIOConfigManager::setActiveSceneLutCccId(const QString &id)
{
    const QString v = id.trimmed();
    if (OcioScenePin *pin = editPin(Slot::SceneLut)) {
        if (pin->sceneLut && pin->sceneLut->cccId == v) return;
        const OcioSceneChain cur = focusedScene();
        pin->sceneLut = OcioScenePin::SceneLut{cur.sceneLutPath, v};
        notePinEdit(focusClipId());
    } else {
        if (m_default.sceneLutCccId == v) return;
        m_default.sceneLutCccId = v;
    }
    publish();
}

void OCIOConfigManager::setActiveDisplayLutPath(const QString &path)
{
    if (m_activeDisplayLutPath == path) return;
    if (!path.isEmpty()) {
        if (!QFileInfo::exists(path)) {
            qWarning("OCIOConfigManager: Display LUT file not found: %s",
                     qPrintable(path));
            return;
        }
        if (!isSupportedLutExtension(path)) {
            qWarning("OCIOConfigManager: unsupported Display LUT extension '%s' "
                     "(expected .cube / .3dl / .csp)",
                     qPrintable(QFileInfo(path).suffix()));
            return;
        }
    }
    m_activeDisplayLutPath = path;
    publish();
}

void OCIOConfigManager::setEngaged(bool b)
{
    if (m_engaged == b) return;
    m_engaged = b;
    publish();
}

// -------- Highlight Knee --------

template <typename Fn>
void OCIOConfigManager::editKnee(Fn &&fn)
{
    if (OcioScenePin *pin = editPin(Slot::Knee)) {
        const OcioSceneChain cur = focusedScene();
        OcioScenePin::Knee k = pin->knee.value_or(OcioScenePin::Knee{
            cur.kneeEnabled, cur.kneeSourceNits, cur.kneeTargetNits, cur.kneeStart});
        const OcioScenePin::Knee before = k;
        fn(k.enabled, k.sourceNits, k.targetNits, k.start);
        if (pin->knee && before.enabled == k.enabled && before.sourceNits == k.sourceNits
            && before.targetNits == k.targetNits && before.start == k.start) {
            return;
        }
        pin->knee = k;
        notePinEdit(focusClipId());
    } else {
        const OcioSceneChain before = m_default;
        fn(m_default.kneeEnabled, m_default.kneeSourceNits, m_default.kneeTargetNits,
           m_default.kneeStart);
        if (before == m_default) return;
    }
    publish(/*knee=*/true);
}

void OCIOConfigManager::setKneeEnabled(bool on)
{
    editKnee([on](bool &e, float &, float &, float &) { e = on; });
}

void OCIOConfigManager::setKneeSourceNits(double nits)
{
    const float v = static_cast<float>(std::clamp(nits, 100.0, 10000.0));
    editKnee([v](bool &, float &src, float &, float &) {
        if (std::abs(src - v) >= 1e-3f) src = v;
    });
}

void OCIOConfigManager::setKneeTargetNits(double nits)
{
    const float v = static_cast<float>(std::clamp(nits, 100.0, 10000.0));
    editKnee([v](bool &, float &, float &tgt, float &) {
        if (std::abs(tgt - v) >= 1e-3f) tgt = v;
    });
}

void OCIOConfigManager::setKneeStart(double fraction)
{
    const float v = fraction < 0.0 ? -1.0f
                                   : static_cast<float>(std::clamp(fraction, 0.0, 0.99));
    editKnee([v](bool &, float &, float &, float &start) {
        if (std::abs(start - v) >= 1e-5f) start = v;
    });
}

LinearStageSettings OCIOConfigManager::linearStageSettings(float gain) const
{
    return specFor(focusedScene()).stage(gain);
}

bool OCIOConfigManager::kneeAvailable() const
{
    if (!m_impl->config) return false;
    OcioSplitTransforms t;
    return OcioChainBuilder::buildSplitTransforms(focusedSpec(), m_impl->config, t);
}

bool OCIOConfigManager::displayIsSdr() const
{
    if (!m_impl->config) return true;
    OcioSplitTransforms t;
    if (!OcioChainBuilder::buildSplitTransforms(focusedSpec(), m_impl->config, t)) {
        return true;
    }
    return t.displayIsSdr;
}

double OCIOConfigManager::kneeStartEffective() const
{
    const LinearStageGpu g = linear_stage::resolve(
        linearStageSettings(1.0f), InterchangeSide::Display, displayIsSdr());
    return g.p0[3] > 0.0f ? g.p1[0] / g.p0[3] : 0.0;
}

double OCIOConfigManager::kneeStartNits() const
{
    const LinearStageGpu g = linear_stage::resolve(
        linearStageSettings(1.0f), InterchangeSide::Display, displayIsSdr());
    return linear_stage::kneeStartNits(g.p1[0], focusedScene().kneeSourceNits);
}

// -------- Per-clip scene chain --------

void OCIOConfigManager::setViewContext(const QString &singleClipId, bool dual,
                                       const QString &clipA, const QString &clipB)
{
    if (m_singleClip == singleClipId && m_dual == dual && m_clipA == clipA
        && m_clipB == clipB) {
        return;
    }
    m_singleClip = singleClipId;
    m_dual       = dual;
    m_clipA      = clipA;
    m_clipB      = clipB;
    emit viewContextChanged();
    publish(/*knee=*/true);   // the focused knee may differ too
}

QString OCIOConfigManager::focusClipId() const
{
    if (!m_dual) return m_singleClip;
    return m_activeTab == 1 ? m_clipB : m_clipA;
}

void OCIOConfigManager::setActiveTab(int tab)
{
    tab = tab == 1 ? 1 : 0;
    if (m_activeTab == tab) return;
    m_activeTab = tab;
    emit viewContextChanged();
    publish(/*knee=*/true);
}

namespace {

std::optional<OcioScenePin::Knee> kneeOf(const OcioSceneChain &c)
{
    return OcioScenePin::Knee{c.kneeEnabled, c.kneeSourceNits, c.kneeTargetNits, c.kneeStart};
}

} // namespace

bool OCIOConfigManager::slotPinned(Slot slot) const
{
    const auto it = m_pins.constFind(focusClipId());
    if (it == m_pins.constEnd()) return false;
    switch (slot) {
    case Slot::Input:    return it->input.has_value();
    case Slot::Look:     return it->look.has_value();
    case Slot::SceneLut: return it->sceneLut.has_value();
    case Slot::Knee:     return it->knee.has_value();
    }
    return false;
}

OcioScenePin *OCIOConfigManager::editPin(Slot slot)
{
    Q_UNUSED(slot);
    // Every clip-side edit stays with the clip on screen, single or dual
    // view alike. With no clip loaded it sets the starting chain.
    const QString clip = focusClipId();
    return clip.isEmpty() ? nullptr : &m_pins[clip];
}

void OCIOConfigManager::notePinEdit(const QString &clipId)
{
    emit pinsChanged(clipId);
    bumpPinsRevision();
}

QString OCIOConfigManager::clipBadge(const QString &clipId) const
{
    const auto it = m_pins.constFind(clipId);
    if (clipId.isEmpty() || it == m_pins.constEnd() || it->empty()) return {};
    if (m_shortNamesConfig != m_configIdentifier) {
        m_shortNames       = colourspace_names::shortNamesFor(colorspaces());
        m_shortNamesConfig = m_configIdentifier;
    }
    QStringList parts;
    if (it->input && !it->input->isEmpty()) {
        parts << m_shortNames.value(*it->input, colourspace_names::shortName(*it->input));
    }
    if (it->look && !it->look->isEmpty()) parts << tr("Look");
    if (it->sceneLut && !it->sceneLut->path.isEmpty()) {
        const QString ext = QFileInfo(it->sceneLut->path).suffix().toLower();
        parts << ((ext == QLatin1String("cc") || ext == QLatin1String("ccc")
                   || ext == QLatin1String("cdl")) ? tr("CDL") : tr("LUT"));
    }
    if (it->knee && it->knee->enabled) parts << tr("Knee");
    // Only empty slots set (e.g. a preset's "no Look"): still its own chain.
    return parts.isEmpty() ? tr("Clip chain") : parts.join(QStringLiteral(" + "));
}

QString OCIOConfigManager::clipBadgeTooltip(const QString &clipId) const
{
    const auto it = m_pins.constFind(clipId);
    if (clipId.isEmpty() || it == m_pins.constEnd() || it->empty()) return {};
    QStringList lines{tr("Set on this clip:")};
    if (it->input) lines << tr("Input: %1").arg(it->input->isEmpty() ? tr("(none)") : *it->input);
    if (it->look)  lines << tr("Look: %1").arg(it->look->isEmpty() ? tr("(none)") : *it->look);
    if (it->sceneLut) {
        lines << tr("Scene LUT: %1").arg(it->sceneLut->path.isEmpty()
                                             ? tr("(none)")
                                             : QFileInfo(it->sceneLut->path).fileName());
    }
    if (it->knee) {
        lines << (it->knee->enabled
                      ? tr("Highlight Knee: on · %1 → %2 nits")
                            .arg(qRound(it->knee->sourceNits)).arg(qRound(it->knee->targetNits))
                      : tr("Highlight Knee: off"));
    }
    return lines.join(QLatin1Char('\n'));
}

void OCIOConfigManager::setSlotPinned(const QString &slotName, bool pinned)
{
    const QString clip = focusClipId();
    if (clip.isEmpty()) return;
    const OcioSceneChain cur = focusedScene();
    OcioScenePin &pin = m_pins[clip];
    if (slotName == QLatin1String("input")) {
        pin.input = pinned ? std::optional<QString>(cur.input) : std::nullopt;
    } else if (slotName == QLatin1String("look")) {
        pin.look = pinned ? std::optional<QString>(cur.look) : std::nullopt;
    } else if (slotName == QLatin1String("sceneLut")) {
        pin.sceneLut = pinned ? std::optional<OcioScenePin::SceneLut>(
                                    OcioScenePin::SceneLut{cur.sceneLutPath, cur.sceneLutCccId})
                              : std::nullopt;
    } else if (slotName == QLatin1String("knee")) {
        pin.knee = pinned ? kneeOf(cur) : std::nullopt;
    } else {
        qWarning("OCIOConfigManager: unknown slot '%s'", qPrintable(slotName));
    }
    if (pin.empty()) m_pins.remove(clip);
    notePinEdit(clip);
    publish(/*knee=*/true);
}

void OCIOConfigManager::copyAChainToB()
{
    if (m_clipA.isEmpty() || m_clipB.isEmpty() || m_clipA == m_clipB) return;
    copyClipChain(m_clipA, {m_clipB});
}

void OCIOConfigManager::setInputForClips(const QStringList &clipIds, const QString &colourspace)
{
    if (!colourspace.isEmpty() && !isValidColorSpace(colourspace)) {
        qWarning("OCIOConfigManager: rejected unknown Input colorspace '%s'",
                 qPrintable(colourspace));
        return;
    }
    bool any = false;
    for (const QString &id : clipIds) {
        if (id.isEmpty()) continue;
        m_pins[id].input = colourspace;
        notePinEdit(id);
        any = true;
    }
    if (any) publish(/*knee=*/true);
}

void OCIOConfigManager::copyClipChain(const QString &fromClipId, const QStringList &toClipIds)
{
    // The source's effective chain (its own settings over the default),
    // so the targets look the same even where the source follows the
    // default.
    const OcioSceneChain from = resolveScene(fromClipId);
    OcioScenePin pin;
    pin.input    = from.input;
    pin.look     = from.look;
    pin.sceneLut = OcioScenePin::SceneLut{from.sceneLutPath, from.sceneLutCccId};
    pin.knee     = kneeOf(from);
    bool any = false;
    for (const QString &id : toClipIds) {
        if (id.isEmpty() || id == fromClipId) continue;
        m_pins[id] = pin;
        notePinEdit(id);
        any = true;
    }
    if (any) publish(/*knee=*/true);
}

void OCIOConfigManager::resetClipChains(const QStringList &clipIds)
{
    bool any = false;
    for (const QString &id : clipIds) {
        if (!m_pins.remove(id)) continue;
        notePinEdit(id);
        any = true;
    }
    if (any) publish(/*knee=*/true);
}

void OCIOConfigManager::replaceAllPins(const QHash<QString, QVariantMap> &pins)
{
    m_pins.clear();
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) {
        const OcioScenePin pin = OcioScenePin::fromVariant(it.value());
        if (!pin.empty()) m_pins.insert(it.key(), pin);
    }
    emit pinsReloaded();
    bumpPinsRevision();
    publish(/*knee=*/true);
}

bool OCIOConfigManager::clipHasPins(const QString &clipId) const
{
    return m_pins.contains(clipId);
}

void OCIOConfigManager::clearClipPins(const QString &clipId)
{
    if (!m_pins.remove(clipId)) return;
    notePinEdit(clipId);
    publish(/*knee=*/true);
}

OcioSceneChain OCIOConfigManager::resolveScene(const QString &clipId) const
{
    OcioSceneChain s = m_default;
    if (clipId.isEmpty()) return s;
    const auto it = m_pins.constFind(clipId);
    if (it == m_pins.constEnd()) return s;
    // A pin naming something the current config lacks (pinned under
    // another config) falls back to the default for that slot.
    if (it->input && (it->input->isEmpty() || isValidColorSpace(*it->input))) {
        s.input = *it->input;
    }
    if (it->look && (it->look->isEmpty() || isValidLook(*it->look))) s.look = *it->look;
    if (it->sceneLut) {
        s.sceneLutPath  = it->sceneLut->path;
        s.sceneLutCccId = it->sceneLut->cccId;
    }
    if (it->knee) {
        s.kneeEnabled    = it->knee->enabled;
        s.kneeSourceNits = it->knee->sourceNits;
        s.kneeTargetNits = it->knee->targetNits;
        s.kneeStart      = it->knee->start;
    }
    return s;
}

OcioChainSpec OCIOConfigManager::specFor(const OcioSceneChain &scene) const
{
    OcioChainSpec spec;
    spec.configPath     = m_impl->config ? m_configIdentifier : QString();
    spec.scene          = scene;
    spec.display        = m_activeDisplay;
    spec.view           = m_activeView;
    spec.displayLutPath = m_activeDisplayLutPath;
    QString d, v;
    if (sdrCaptureDisplayView(&d, &v)) {
        spec.sdrDisplay = d;
        spec.sdrView    = v;
    }
    return spec;
}

OcioChainSpec OCIOConfigManager::focusedSpec() const
{
    return specFor(focusedScene());
}

OcioChainSpec OCIOConfigManager::specForClip(const QString &clipId) const
{
    return specFor(resolveScene(clipId));
}

std::shared_ptr<const OcioChainSnapshot> OCIOConfigManager::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void OCIOConfigManager::publish(bool knee)
{
    // A dual-view edit that changed nothing can leave an empty pin.
    m_pins.removeIf([](const auto &it) { return it.value().empty(); });
    auto snap = std::make_shared<OcioChainSnapshot>();
    snap->engaged = m_engaged;
    snap->dual    = m_dual;
    // The display side and the SDR capture pair are shared: resolve once.
    const OcioChainSpec base = specFor(m_default);
    auto withScene = [&base](const OcioSceneChain &scene) {
        OcioChainSpec s = base;
        s.scene = scene;
        return s;
    };
    snap->single = withScene(resolveScene(m_singleClip));
    snap->a      = withScene(resolveScene(m_clipA));
    snap->b      = withScene(resolveScene(m_clipB));
    // Distinct shaders only (knee values are uniforms); the on-screen
    // chains first, then the rest — capped, the renderers' caches are
    // small.
    constexpr size_t kMaxWarm = 8;
    auto addWarm = [&snap](const OcioChainSpec &spec) {
        if (!spec.complete() || snap->warm.size() >= kMaxWarm) return;
        for (const OcioChainSpec &w : snap->warm) if (w.sameShader(spec)) return;
        snap->warm.push_back(spec);
    };
    addWarm(snap->single);
    addWarm(snap->b);
    addWarm(base);
    for (auto it = m_pins.constBegin(); it != m_pins.constEnd(); ++it) {
        addWarm(withScene(resolveScene(it.key())));
    }
    snap->generation = m_activeChainGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(snap);
    }
    if (knee) m_stageGeneration.fetch_add(1, std::memory_order_acq_rel);
    emit activeChainChanged();
}

// -------- private --------

void OCIOConfigManager::resetActiveDefaults()
{
    // Engagement is the user's persistent intent ("I want OCIO
    // active") and is orthogonal to which config / chain is loaded.
    // We don't touch m_engaged here so config switches preserve the
    // user's engage state — they pick a new config and the new
    // chain's defaults render live without having to re-flip the
    // toggle. The two exceptions are (a) the no-config branch below,
    // where there's no chain to engage with, and (b) the constructor's
    // default initializer in the header (engaged = false on launch).
    if (!m_impl->config) {
        m_default = OcioSceneChain{};
        m_activeDisplay.clear();
        m_activeView.clear();
        m_activeDisplayLutPath.clear();
        m_engaged = false;
        publish(/*knee=*/true);
        return;
    }
    // The knee settings are the user's, not the config's: keep them.
    const OcioSceneChain prev = m_default;
    m_default = OcioSceneChain{};
    m_default.kneeEnabled    = prev.kneeEnabled;
    m_default.kneeSourceNits = prev.kneeSourceNits;
    m_default.kneeTargetNits = prev.kneeTargetNits;
    m_default.kneeStart      = prev.kneeStart;

    // Default Input — try 'color_picking' role (sRGB-equivalent in
    // most configs); fall back to 'scene_linear'; then to the first
    // colorspace.
    if (const char *cs = m_impl->config->getRoleColorSpace("color_picking"); cs && *cs) {
        m_default.input = QString::fromUtf8(cs);
    } else if (const char *cs = m_impl->config->getRoleColorSpace("scene_linear"); cs && *cs) {
        m_default.input = QString::fromUtf8(cs);
    } else if (m_impl->config->getNumColorSpaces() > 0) {
        m_default.input = QString::fromUtf8(
            m_impl->config->getColorSpaceNameByIndex(0));
    }

    // Default Display — config's default display.
    if (const char *d = m_impl->config->getDefaultDisplay(); d && *d) {
        m_activeDisplay = QString::fromUtf8(d);
    } else if (m_impl->config->getNumDisplays() > 0) {
        m_activeDisplay = QString::fromUtf8(m_impl->config->getDisplay(0));
    } else {
        m_activeDisplay.clear();
    }

    // Default View — prefer "Un-tone-mapped" when available (the
    // OCIO 2.5 default config's identity-ish view for SDR content),
    // else the config's getDefaultView() (often an ACES tone-mapped
    // SDR view, which dims in-range content noticeably). Phase 2.4
    // lets users pick anything else from the slot-machine UI.
    if (!m_activeDisplay.isEmpty()) {
        const QStringList views = viewsForDisplay(m_activeDisplay);
        const QString preferUntonemapped = QStringLiteral("Un-tone-mapped");
        if (views.contains(preferUntonemapped)) {
            m_activeView = preferUntonemapped;
        } else {
            const QByteArray d = m_activeDisplay.toUtf8();
            if (const char *v = m_impl->config->getDefaultView(d.constData());
                v && *v) {
                m_activeView = QString::fromUtf8(v);
            } else {
                m_activeView = views.isEmpty() ? QString() : views.first();
            }
        }
    } else {
        m_activeView.clear();
    }

    m_activeDisplayLutPath.clear();
    // m_engaged intentionally preserved — see comment at top of fn.
    publish(/*knee=*/true);
}

bool OCIOConfigManager::isValidColorSpace(const QString &name) const
{
    if (!m_impl->config || name.isEmpty()) return false;
    OCIO::ConstColorSpaceRcPtr cs = m_impl->config->getColorSpace(name.toUtf8().constData());
    return cs != nullptr;
}

bool OCIOConfigManager::isValidDisplay(const QString &name) const
{
    if (!m_impl->config || name.isEmpty()) return false;
    for (int i = 0; i < m_impl->config->getNumDisplays(); ++i) {
        if (name == QString::fromUtf8(m_impl->config->getDisplay(i))) return true;
    }
    return false;
}

bool OCIOConfigManager::isValidViewForDisplay(const QString &display,
                                              const QString &view) const
{
    if (!m_impl->config || display.isEmpty() || view.isEmpty()) return false;
    const QByteArray d = display.toUtf8();
    for (int i = 0; i < m_impl->config->getNumViews(d.constData()); ++i) {
        if (view == QString::fromUtf8(m_impl->config->getView(d.constData(), i))) {
            return true;
        }
    }
    return false;
}

bool OCIOConfigManager::isValidLook(const QString &name) const
{
    if (!m_impl->config || name.isEmpty()) return false;
    return m_impl->config->getLook(name.toUtf8().constData()) != nullptr;
}

} // namespace qcv
