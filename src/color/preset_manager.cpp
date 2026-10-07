#include "preset_manager.h"
#include "mincolor_engine.h"

#include "ocio_config_manager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtLogging>

namespace qcv {

PresetManager::PresetManager(OCIOConfigManager *ocio, MinColorEngine *minColor, QObject *parent)
    : QObject(parent), m_ocio(ocio), m_minColor(minColor)
{
    loadBuiltIns();
    loadMinColorBuiltIns();
    loadUserPresetsFromDisk();
    if (m_minColor) {
        connect(m_minColor, &MinColorEngine::chainChanged,
                this, &PresetManager::onActiveChainChanged);
    }

    // Track invalidation: any chain edit that didn't come from
    // applyPreset itself toggles us into "modified" state; switching
    // configs clears the active preset entirely (the slots reset to
    // the new config's defaults so the preset name no longer
    // describes what's loaded).
    if (m_ocio) {
        connect(m_ocio, &OCIOConfigManager::activeChainChanged,
                this, &PresetManager::onActiveChainChanged);
        connect(m_ocio, &OCIOConfigManager::configChanged,
                this, &PresetManager::onConfigChanged);
        // The Highlight Knee is part of the chain a preset recalls.
        connect(m_ocio, &OCIOConfigManager::kneeChanged,
                this, &PresetManager::onActiveChainChanged);
    }
}

PresetManager::~PresetManager() = default;

void PresetManager::loadBuiltIns()
{
    // Built-ins ported from the old app (src/app/color_presets.cpp).
    // Phase 2.5e.1.5 pivot: the [Config] prefix on names was hard
    // to read in the narrow Preset reel. Now the Preset struct
    // carries a `section` field; the QML reel uses ListView's
    // section.property to render group headers ("Blender 5.1",
    // "ACES 2.0", etc.) and the names themselves are short and
    // descriptive.
    //
    // HDR presets (entries with EDR / PQ / HDR in the name) load
    // their chains fine but render clamped until Phase 2.6 ships
    // extended-linear swapchain support.

    auto P = [](const QString &name, const QString &section,
                const QString &cfg, const QString &input,
                const QString &look, const QString &output, const QString &view,
                Preset::Kind kind = Preset::SdrSrgb) {
        return Preset{ name, section, cfg, input, look,
                       /*sceneLutPath*/ {}, output, view,
                       /*displayLutPath*/ {}, /*builtIn*/ true, kind };
    };

    // CONFIG UPGRADE: every built-in preset names a config (by friendly
    // name) and its colourspace / display / view by string. After
    // replacing a bundled config, check each still resolves — a stale
    // name leaves the preset unloadable (assets/OCIO/patches/README.md,
    // "Names QCView depends on").
    const QString secBuiltIn   = QStringLiteral("Built-in");
    const QString secBlender52 = QStringLiteral("Blender 5.2");
    const QString secBlender   = QStringLiteral("Blender 5.1");
    const QString secAces2     = QStringLiteral("ACES 2.0");
    const QString secAces1     = QStringLiteral("ACES 1.3");

    m_presets = {
        // --- Passthrough ---
        Preset{ QStringLiteral("None (Passthrough)"), secBuiltIn,
                {}, {}, {}, {}, {}, {}, {}, true, Preset::Universal },

        // --- Blender 5.2 SDR ---
        // Same literals as the 5.1 section below — every referenced
        // colorspace/display/view name is unchanged between the two
        // configs. 5.1 sections stay because user presets store raw
        // configName strings with no migration path.
        P(QStringLiteral("Rec.709 → sRGB Standard"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Rec.1886"),
          {}, QStringLiteral("sRGB"), QStringLiteral("Standard")),

        P(QStringLiteral("Linear Rec.709 → sRGB Standard"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("sRGB"), QStringLiteral("Standard")),

        P(QStringLiteral("Linear Rec.709 → sRGB AgX"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("sRGB"), QStringLiteral("AgX")),

        P(QStringLiteral("Linear Rec.709 → Display P3"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Display P3"), QStringLiteral("Standard"),
          Preset::SdrP3),

        // --- Blender 5.2 HDR (linear-light EDR) ---
        // sRGB-primaries variant works on macOS EDR + Windows scRGB.
        P(QStringLiteral("Rec.709 → EDR sRGB 1000 nits"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Linear sRGB EDR"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits"),
          Preset::HdrEdrSrgb),

        // P3-primaries variant — macOS only.
        P(QStringLiteral("Rec.709 → EDR P3 1000 nits"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Linear P3 EDR"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits"),
          Preset::HdrEdrP3),

        // --- Blender 5.2 ST2084-P3 (PQ P3-D65 masters) ---
        // Display-referred HDR deliverables (Resolve "P3-D65 ST2084").
        // Standard views are colorimetric — no tonemap: 100 nits lands
        // on SDR white (brighter clips on the SDR outputs), 1000 nits
        // on 10.0 in the EDR outputs, and PQ output round-trips.
        // Names stay unique — applyPreset() looks presets up by name.
        P(QStringLiteral("ST2084-P3 → sRGB Standard"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("ST2084-P3-D65"),
          {}, QStringLiteral("sRGB"), QStringLiteral("Standard")),

        P(QStringLiteral("ST2084-P3 → Rec.1886 Standard"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("ST2084-P3-D65"),
          {}, QStringLiteral("Rec.1886"), QStringLiteral("Standard")),

        // sRGB-primaries variant works on macOS EDR + Windows scRGB.
        P(QStringLiteral("ST2084-P3 → EDR sRGB"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("ST2084-P3-D65"),
          {}, QStringLiteral("Linear sRGB EDR"),
          QStringLiteral("Standard (No Tonemap)"),
          Preset::HdrEdrSrgb),

        // P3-primaries variant — macOS only.
        P(QStringLiteral("ST2084-P3 → EDR P3"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("ST2084-P3-D65"),
          {}, QStringLiteral("Linear P3 EDR"),
          QStringLiteral("Standard (No Tonemap)"),
          Preset::HdrEdrP3),

        // HDR10 swapchain — Windows / Linux.
        P(QStringLiteral("ST2084-P3 → Rec.2100-PQ HDR"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("ST2084-P3-D65"),
          {}, QStringLiteral("Rec.2100-PQ"), QStringLiteral("Standard"),
          Preset::HdrPq),

        // --- Blender 5.1 SDR ---
        P(QStringLiteral("Rec.709 → sRGB Standard"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Rec.1886"),
          {}, QStringLiteral("sRGB"), QStringLiteral("Standard")),

        P(QStringLiteral("Linear Rec.709 → sRGB Standard"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("sRGB"), QStringLiteral("Standard")),

        P(QStringLiteral("Linear Rec.709 → sRGB AgX"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("sRGB"), QStringLiteral("AgX")),

        P(QStringLiteral("Linear Rec.709 → Display P3"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Display P3"), QStringLiteral("Standard"),
          Preset::SdrP3),

        // --- Blender 5.1 HDR (linear-light EDR) ---
        // sRGB-primaries variant works on macOS EDR + Windows scRGB.
        P(QStringLiteral("Rec.709 → EDR sRGB 1000 nits"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Linear sRGB EDR"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits"),
          Preset::HdrEdrSrgb),

        // P3-primaries variant — macOS only.
        P(QStringLiteral("Rec.709 → EDR P3 1000 nits"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Linear P3 EDR"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits"),
          Preset::HdrEdrP3),

        // --- ACES 2.0 SDR (colorimetric) ---
        P(QStringLiteral("ACEScg → sRGB"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("ACEScg"),
          {}, QStringLiteral("sRGB - Display"),
          QStringLiteral("Video (colorimetric)")),

        P(QStringLiteral("ACEScg → Display P3"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("ACEScg"),
          {}, QStringLiteral("Display P3 - Display"),
          QStringLiteral("Video (colorimetric)"),
          Preset::SdrP3),

        P(QStringLiteral("sRGB → Rec.2100-PQ HDR"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("sRGB - Display"),
          {}, QStringLiteral("Rec.2100-PQ - Display"),
          QStringLiteral("Video (colorimetric)"),
          Preset::HdrPq),

        P(QStringLiteral("Rec.709 → Rec.2100-PQ HDR"), secAces2,
          QStringLiteral("ACES 2.0"),
          QStringLiteral("Rec.1886 Rec.709 - Display"),
          {}, QStringLiteral("Rec.2100-PQ - Display"),
          QStringLiteral("Video (colorimetric)"),
          Preset::HdrPq),

        // --- ACES 2.0 HDR (linear-light EDR) ---
        // sRGB-primaries variant works on macOS EDR + Windows scRGB.
        P(QStringLiteral("ACEScg → EDR sRGB 1000 nits"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("ACEScg"),
          {}, QStringLiteral("Linear sRGB EDR - Display"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits (P3 D65)"),
          Preset::HdrEdrSrgb),

        // P3-primaries variant — macOS only.
        P(QStringLiteral("ACEScg → EDR P3 1000 nits"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("ACEScg"),
          {}, QStringLiteral("Linear P3 EDR - Display"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits (P3 D65)"),
          Preset::HdrEdrP3),

        // --- ACES 1.3 ---
        // ACES 1.3 calls ACEScg "lin_ap1" in its OCIO config — the
        // input slot keeps that name (the chain build references it),
        // but the user-facing preset name uses the more recognizable
        // "ACEScg" label.
        P(QStringLiteral("ACEScg → sRGB"), secAces1,
          QStringLiteral("ACES 1.3"), QStringLiteral("lin_ap1"),
          {}, QStringLiteral("srgb_display"),
          QStringLiteral("ACES 1.0 - SDR Video")),

        P(QStringLiteral("aces2065-1 → sRGB"), secAces1,
          QStringLiteral("ACES 1.3"), QStringLiteral("aces2065_1"),
          {}, QStringLiteral("srgb_display"),
          QStringLiteral("ACES 1.0 - SDR Video")),
    };

    emit presetsChanged();
}

int PresetManager::activeEngine() const
{
    return m_ocio ? m_ocio->engine() : 0;
}

// minColor's built-ins: one per display encoding, every one of them
// un-tone-mapped (an OpenDRT look is only ever an explicit choice —
// chris, 2026-10-07), with Blender's AgX as one extra on the SDR encode.
void PresetManager::loadMinColorBuiltIns()
{
    struct Seed { const char *name; int display; Preset::Kind kind; bool agx; };
    static const Seed kSeeds[] = {
        {"sRGB 2.2 / Rec.709",           1, Preset::SdrSrgb,    false},
        {"Rec.1886 / Rec.709",           0, Preset::SdrSrgb,    false},
        {"Display P3",                   2, Preset::SdrP3,      false},
        {"Rec.2100 PQ (P3 limited)",     6, Preset::HdrPq,      false},
        {"Rec.2100 HLG (P3 limited)",    7, Preset::HdrPq,      false},
        {"Linear Rec.709 (EDR)",        13, Preset::HdrEdrSrgb, false},
        {"Linear Rec.2020 (EDR)",       12, Preset::HdrEdrP3,   false},
        {"AgX → sRGB 2.2",               1, Preset::SdrSrgb,    true},
    };
    for (const Seed &sd : kSeeds) {
        MinColorChain chain;   // Input Rec.709 / Rec.1886, knee off, un-tone-mapped
        chain.output.display = sd.display;
        chain.agx.enabled    = sd.agx;
        Preset p;
        p.name     = QString::fromUtf8(sd.name);
        p.section  = QStringLiteral("minColor");
        p.builtIn  = true;
        p.kind     = sd.kind;
        p.engine   = 1;
        p.minColor = chain.toVariant();
        m_presets.append(p);
    }
}

QVariantList PresetManager::availablePresetEntries() const
{
    QVariantList out;
    out.reserve(m_presets.size());
    const int engine = activeEngine();
    for (const Preset &p : m_presets) {
        if (p.engine != engine) continue;
        QVariantMap entry;
        entry[QStringLiteral("name")]    = p.name;
        entry[QStringLiteral("section")] = p.section;
        entry[QStringLiteral("kind")]    = static_cast<int>(p.kind);
        out.append(entry);
    }
    return out;
}

QStringList PresetManager::availablePresets() const
{
    QStringList out;
    out.reserve(m_presets.size());
    const int engine = activeEngine();
    for (const Preset &p : m_presets) if (p.engine == engine) out << p.name;
    return out;
}

const PresetManager::Preset *PresetManager::findByName(const QString &name) const
{
    for (const Preset &p : m_presets) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

bool PresetManager::applyPreset(const QString &name)
{
    if (!m_ocio) return false;
    const Preset *p = findByName(name);
    if (!p) {
        qWarning("PresetManager: unknown preset '%s'", qPrintable(name));
        return false;
    }

    m_applying = true;

    // A minColor preset: the whole chain onto the engine, the engine
    // selected, On — the same deliberate "I want this look" as below.
    if (p->engine == 1) {
        if (!m_minColor) { m_applying = false; return false; }
        m_minColor->setChain(MinColorChain::fromVariant(p->minColor));
        m_ocio->setEngine(1);
        m_ocio->setEngaged(true);
        m_applying = false;
        bool changedMc = false;
        if (m_activePresetName != name) { m_activePresetName = name; changedMc = true; }
        if (m_modified) { m_modified = false; emit modifiedChanged(); }
        if (changedMc) emit activePresetChanged();
        return true;
    }
    if (m_ocio->engine() != 0) m_ocio->setEngine(0);

    // Switch config first if needed — the input/display/view names
    // in the preset are interpreted in the target config's name
    // space, so the config has to be live before we set them.
    if (!p->configName.isEmpty()
        && p->configName != m_ocio->activeConfigName()) {
        if (!m_ocio->setActiveConfig(p->configName)) {
            qWarning("PresetManager: preset '%s' references config "
                     "'%s' which isn't available — slots not applied",
                     qPrintable(name), qPrintable(p->configName));
            m_applying = false;
            return false;
        }
    }

    // Apply each slot. Setters now accept empty as "clear" (Guide
    // 05 D6 — passthrough on absence) so the "None (Passthrough)"
    // preset wipes all four colorspace-name slots. Same split as the
    // panel, in single and dual view: the clip half (Input, Look, Scene
    // LUT, knee) lands on the selected clip, the view half (Output,
    // View, Display LUT) on the shared View.
    m_ocio->setActiveInput(p->input);
    m_ocio->setActiveLook(p->look);
    m_ocio->setActiveDisplay(p->output);
    m_ocio->setActiveView(p->view);
    m_ocio->setActiveSceneLutPath(p->sceneLutPath);
    m_ocio->setActiveSceneLutCccId(p->sceneLutCccId);
    m_ocio->setActiveDisplayLutPath(p->displayLutPath);
    m_ocio->setKneeSourceNits(p->kneeSourceNits);
    m_ocio->setKneeTargetNits(p->kneeTargetNits);
    m_ocio->setKneeStart(p->kneeStart);
    m_ocio->setKneeEnabled(p->kneeEnabled);

    // Auto-engage (Phase 2.5e.1.5): the user picking a preset is a
    // deliberate "I want this look" action — it shouldn't require a
    // separate Engage flip. They can still disengage manually if
    // they want to compare to the raw image.
    m_ocio->setEngaged(true);

    m_applying = false;

    bool changed = false;
    if (m_activePresetName != name) {
        m_activePresetName = name;
        changed = true;
    }
    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    if (changed) emit activePresetChanged();
    return true;
}

bool PresetManager::currentMatchesPreset(const Preset &p) const
{
    if (!m_ocio) return false;
    if (p.engine == 1) {
        return m_minColor && m_ocio->engine() == 1
            && m_minColor->chain() == MinColorChain::fromVariant(p.minColor);
    }
    if (m_ocio->engine() != 0) return false;
    // configName comparison handles the empty-config case for the
    // "None (Passthrough)" preset (configName is empty; current
    // configName is whatever's loaded — they won't match, so None
    // is "modified" the moment any other config is active. That's
    // correct: None means literally nothing is engaged, and we
    // can't represent that state while a config is loaded with
    // non-default slots).
    return m_ocio->activeConfigName()      == p.configName
        && m_ocio->activeInput()           == p.input
        && m_ocio->activeLook()            == p.look
        && m_ocio->activeDisplay()         == p.output
        && m_ocio->activeView()            == p.view
        && m_ocio->activeSceneLutPath()    == p.sceneLutPath
        && m_ocio->activeDisplayLutPath()  == p.displayLutPath
        && m_ocio->activeSceneLutCccId()   == p.sceneLutCccId
        && m_ocio->kneeEnabled()           == p.kneeEnabled
        // Knee parameters only matter while it's on.
        && (!p.kneeEnabled
            || (std::abs(m_ocio->kneeSourceNits() - p.kneeSourceNits) < 0.5
                && std::abs(m_ocio->kneeTargetNits() - p.kneeTargetNits) < 0.5
                && std::abs(m_ocio->kneeStart() - p.kneeStart) < 1e-4));
}

void PresetManager::onActiveChainChanged()
{
    if (m_applying) return;
    // The engine segment changes which presets the reel lists.
    const int engine = activeEngine();
    if (engine != m_listedEngine) {
        m_listedEngine = engine;
        emit presetsChanged();
    }
    if (m_activePresetName.isEmpty()) return;
    const Preset *p = findByName(m_activePresetName);
    if (!p) return;
    // An engine switch leaves the other engine's preset behind: the
    // name no longer describes what's on screen.
    if (p->engine != engine) {
        m_activePresetName.clear();
        emit activePresetChanged();
        if (m_modified) { m_modified = false; emit modifiedChanged(); }
        return;
    }
    const bool nowModified = !currentMatchesPreset(*p);
    if (nowModified != m_modified) {
        m_modified = nowModified;
        emit modifiedChanged();
    }
}

void PresetManager::onConfigChanged()
{
    if (m_applying) return;
    // Manual config switch: the active preset's slots are written
    // in the *old* config's namespace, so the preset name no longer
    // describes what's loaded. Clear it. (Switching configs as part
    // of a preset apply goes through m_applying=true and is
    // unaffected.)
    if (!m_activePresetName.isEmpty()) {
        m_activePresetName.clear();
        emit activePresetChanged();
    }
    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
}

void PresetManager::stepPreset(int delta)
{
    const QStringList names = availablePresets();   // the active engine's
    if (names.isEmpty()) return;
    int idx = names.indexOf(m_activePresetName);
    if (idx < 0) idx = 0;
    int next = (idx + delta) % names.size();
    if (next < 0) next += names.size();
    applyPreset(names[next]);
}

// ---- Phase 2.5e.2: save + persistence ----

PresetManager::Preset *PresetManager::findByNameMutable(const QString &name)
{
    for (Preset &p : m_presets) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

bool PresetManager::activeIsBuiltIn() const
{
    if (const Preset *p = findByName(m_activePresetName)) {
        return p->builtIn;
    }
    return false;
}

namespace {

// Derive a Preset::Kind from the OCIO output / display name. Used for
// user-saved presets (which don't ship with a kind in the JSON); the
// built-ins set it explicitly in loadBuiltIns(). Same heuristic shape
// as Guide 06 §3's star-decoration matcher.
PresetManager::Preset::Kind inferKindFromOutput(const QString &output)
{
    if (output.isEmpty()) return PresetManager::Preset::Universal;
    const QString lo = output.toLower();
    const bool hasPq      = lo.contains(QStringLiteral("pq"))
                         || lo.contains(QStringLiteral("st.2084"))
                         || lo.contains(QStringLiteral("st2084"))
                         || lo.contains(QStringLiteral("rec.2100-pq"))
                         || lo.contains(QStringLiteral("rec2100-pq"))
                         || lo.contains(QStringLiteral("hdr10"));
    if (hasPq) return PresetManager::Preset::HdrPq;
    const bool hasEdr     = lo.contains(QStringLiteral("edr"))
                         || lo.contains(QStringLiteral("linear"))
                         || lo.contains(QStringLiteral("extended"));
    if (hasEdr) {
        // Split by primaries — P3-tagged linear EDR is macOS-only;
        // sRGB / Rec.709 linear EDR works on Windows scRGB too.
        const bool hasP3 = lo.contains(QStringLiteral("p3"));
        return hasP3 ? PresetManager::Preset::HdrEdrP3
                     : PresetManager::Preset::HdrEdrSrgb;
    }
    // SDR — split on primaries again so Display P3 SDR Outputs only
    // light up in the SDR Display P3 swapchain mode, and sRGB / 709 /
    // 1886 only light up in the SDR sRGB mode.
    const bool hasP3 = lo.contains(QStringLiteral("p3"))
                    && !lo.contains(QStringLiteral("dci"));
    return hasP3 ? PresetManager::Preset::SdrP3
                 : PresetManager::Preset::SdrSrgb;
}

} // namespace

PresetManager::Preset
PresetManager::captureCurrentAsPreset(const QString &name) const
{
    Preset p;
    p.name = name;
    p.section = QStringLiteral("Custom");
    p.builtIn = false;
    p.engine = activeEngine();
    if (p.engine == 1 && m_minColor) {
        p.minColor = m_minColor->chain().toVariant();
        const int kind = m_minColor->displayKindOf(m_minColor->chain().output.display);
        p.kind = kind == 1 || kind == 2 ? Preset::HdrPq
               : kind == 3 ? Preset::HdrEdrSrgb : Preset::SdrSrgb;
        return p;
    }
    if (m_ocio) {
        p.configName     = m_ocio->activeConfigName();
        p.input          = m_ocio->activeInput();
        p.look           = m_ocio->activeLook();
        p.output         = m_ocio->activeDisplay();
        p.view           = m_ocio->activeView();
        p.sceneLutPath   = m_ocio->activeSceneLutPath();
        p.displayLutPath = m_ocio->activeDisplayLutPath();
        p.sceneLutCccId  = m_ocio->activeSceneLutCccId();
        p.kneeEnabled    = m_ocio->kneeEnabled();
        p.kneeSourceNits = m_ocio->kneeSourceNits();
        p.kneeTargetNits = m_ocio->kneeTargetNits();
        p.kneeStart      = m_ocio->kneeStart();
    }
    p.kind = inferKindFromOutput(p.output);
    return p;
}

bool PresetManager::saveCurrent()
{
    if (m_activePresetName.isEmpty()) {
        qWarning("PresetManager::saveCurrent: no active preset — call saveAs");
        return false;
    }
    Preset *existing = findByNameMutable(m_activePresetName);
    if (!existing) {
        qWarning("PresetManager::saveCurrent: active preset '%s' is missing",
                 qPrintable(m_activePresetName));
        return false;
    }
    if (existing->builtIn) {
        qWarning("PresetManager::saveCurrent: '%s' is built-in — call saveAs",
                 qPrintable(existing->name));
        return false;
    }
    Preset captured = captureCurrentAsPreset(existing->name);
    captured.section = existing->section;  // keep "Custom"
    *existing = captured;
    if (!writeUserPresetsToDisk()) return false;

    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    emit presetsChanged();
    return true;
}

bool PresetManager::saveAs(const QString &name)
{
    if (name.trimmed().isEmpty()) {
        qWarning("PresetManager::saveAs: empty name rejected");
        return false;
    }
    if (findByName(name)) {
        qWarning("PresetManager::saveAs: '%s' already exists",
                 qPrintable(name));
        return false;
    }
    Preset p = captureCurrentAsPreset(name);
    m_presets.append(p);
    if (!writeUserPresetsToDisk()) {
        // Roll back the in-memory append so disk + memory stay in sync.
        m_presets.removeLast();
        return false;
    }

    m_activePresetName = name;
    emit presetsChanged();
    emit activePresetChanged();
    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    return true;
}

bool PresetManager::deleteCurrent()
{
    if (m_activePresetName.isEmpty()) {
        qWarning("PresetManager::deleteCurrent: no active preset");
        return false;
    }
    int idx = -1;
    for (int i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name == m_activePresetName) { idx = i; break; }
    }
    if (idx < 0) {
        qWarning("PresetManager::deleteCurrent: active preset '%s' is missing",
                 qPrintable(m_activePresetName));
        return false;
    }
    if (m_presets[idx].builtIn) {
        qWarning("PresetManager::deleteCurrent: '%s' is built-in — cannot delete",
                 qPrintable(m_presets[idx].name));
        return false;
    }

    // Snapshot for rollback on disk-write failure — and, on success,
    // for the undo-toast (undoDeleteLast).
    const Preset removed = m_presets.takeAt(idx);
    if (!writeUserPresetsToDisk()) {
        m_presets.insert(idx, removed);
        return false;
    }
    m_lastDeleted      = removed;
    m_lastDeletedIndex = idx;
    m_hasLastDeleted   = true;

    m_activePresetName.clear();
    emit presetsChanged();
    emit activePresetChanged();
    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    return true;
}

QString PresetManager::undoDeleteLast()
{
    if (!m_hasLastDeleted) return {};
    if (findByName(m_lastDeleted.name)) {
        // The name was reused since the delete (re-save / save-as) —
        // restoring would collide. The newer preset wins; drop the
        // snapshot for good.
        qWarning("PresetManager::undoDeleteLast: '%s' exists again — "
                 "not restoring", qPrintable(m_lastDeleted.name));
        m_hasLastDeleted = false;
        return {};
    }

    const int idx = qBound(0, m_lastDeletedIndex,
                           static_cast<int>(m_presets.size()));
    m_presets.insert(idx, m_lastDeleted);
    if (!writeUserPresetsToDisk()) {
        m_presets.removeAt(idx);   // keep the snapshot — retry possible
        return {};
    }
    m_hasLastDeleted = false;

    // Reinstate the pre-delete selection. deleteCurrent never touched
    // the live OCIO chain, so don't re-apply — just recompute whether
    // the chain still matches (the user may have tweaked reels in
    // between, which correctly reads as "modified").
    m_activePresetName = m_lastDeleted.name;
    emit presetsChanged();
    emit activePresetChanged();
    const bool nowModified = !currentMatchesPreset(m_lastDeleted);
    if (m_modified != nowModified) {
        m_modified = nowModified;
        emit modifiedChanged();
    }
    return m_lastDeleted.name;
}

QString PresetManager::userPresetsFilePath() const
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QStringLiteral("/color-presets.json");
}

void PresetManager::loadUserPresetsFromDisk()
{
    const QString path = userPresetsFilePath();
    QFile f(path);
    if (!f.exists()) return;          // first launch — no user presets yet
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("PresetManager: cannot open %s: %s",
                 qPrintable(path), qPrintable(f.errorString()));
        return;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning("PresetManager: %s parse error: %s",
                 qPrintable(path), qPrintable(err.errorString()));
        return;
    }
    const QJsonObject root = doc.object();
    const int schema = root.value(QStringLiteral("schema")).toInt(0);
    if (schema != 1) {
        qWarning("PresetManager: %s has unsupported schema %d (expected 1)",
                 qPrintable(path), schema);
        return;
    }
    const QJsonArray arr = root.value(QStringLiteral("user_presets")).toArray();
    int loaded = 0;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const QString n = o.value(QStringLiteral("name")).toString();
        if (n.isEmpty()) continue;
        if (findByName(n)) {
            qWarning("PresetManager: skipping duplicate user preset '%s'",
                     qPrintable(n));
            continue;
        }
        // Local variable name avoids `slots` — Qt MOC defines that
        // as a no-op macro, producing parse errors on bare use.
        const QJsonObject slotsObj = o.value(QStringLiteral("slots")).toObject();
        Preset p;
        p.name           = n;
        p.section        = QStringLiteral("Custom");
        p.builtIn        = false;
        p.configName     = o.value(QStringLiteral("configName")).toString();
        p.input          = slotsObj.value(QStringLiteral("input")).toString();
        p.look           = slotsObj.value(QStringLiteral("look")).toString();
        p.output         = slotsObj.value(QStringLiteral("output")).toString();
        p.view           = slotsObj.value(QStringLiteral("view")).toString();
        p.sceneLutPath   = slotsObj.value(QStringLiteral("scene_lut")).toString();
        p.displayLutPath = slotsObj.value(QStringLiteral("display_lut")).toString();
        p.sceneLutCccId  = slotsObj.value(QStringLiteral("scene_lut_cccid")).toString();
        const QJsonObject kneeObj = slotsObj.value(QStringLiteral("knee")).toObject();
        p.kneeEnabled    = kneeObj.value(QStringLiteral("enabled")).toBool(false);
        p.kneeSourceNits = kneeObj.value(QStringLiteral("source_nits")).toDouble(1000.0);
        p.kneeTargetNits = kneeObj.value(QStringLiteral("target_nits")).toDouble(1000.0);
        p.kneeStart      = kneeObj.value(QStringLiteral("start")).toDouble(-1.0);
        p.kind           = inferKindFromOutput(p.output);
        p.engine         = o.value(QStringLiteral("engine")).toInt(0) == 1 ? 1 : 0;
        if (p.engine == 1) {
            p.minColor = o.value(QStringLiteral("minColor")).toObject().toVariantMap();
            const int d = p.minColor.value(QStringLiteral("output")).toMap()
                              .value(QStringLiteral("display"), 1).toInt();
            const int kind = m_minColor ? m_minColor->displayKindOf(d) : 0;
            p.kind = kind == 1 || kind == 2 ? Preset::HdrPq
                   : kind == 3 ? Preset::HdrEdrSrgb : Preset::SdrSrgb;
        }
        m_presets.append(p);
        ++loaded;
    }
    if (loaded > 0) {
        qInfo("PresetManager: loaded %d user preset(s) from %s",
              loaded, qPrintable(path));
        emit presetsChanged();
    }
}

bool PresetManager::writeUserPresetsToDisk() const
{
    const QString path = userPresetsFilePath();
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) {
        qWarning("PresetManager: cannot create preset dir %s",
                 qPrintable(dir));
        return false;
    }

    QJsonArray arr;
    for (const Preset &p : m_presets) {
        if (p.builtIn) continue;       // only user presets persist
        QJsonObject slotsObj;
        slotsObj[QStringLiteral("input")]        = p.input;
        slotsObj[QStringLiteral("look")]         = p.look;
        slotsObj[QStringLiteral("output")]       = p.output;
        slotsObj[QStringLiteral("view")]         = p.view;
        slotsObj[QStringLiteral("scene_lut")]    = p.sceneLutPath;
        slotsObj[QStringLiteral("display_lut")]  = p.displayLutPath;
        if (!p.sceneLutCccId.isEmpty()) {
            slotsObj[QStringLiteral("scene_lut_cccid")] = p.sceneLutCccId;
        }
        if (p.kneeEnabled) {
            QJsonObject kneeObj;
            kneeObj[QStringLiteral("enabled")]     = true;
            kneeObj[QStringLiteral("source_nits")] = p.kneeSourceNits;
            kneeObj[QStringLiteral("target_nits")] = p.kneeTargetNits;
            kneeObj[QStringLiteral("start")]       = p.kneeStart;
            slotsObj[QStringLiteral("knee")]       = kneeObj;
        }

        QJsonObject obj;
        obj[QStringLiteral("name")]       = p.name;
        obj[QStringLiteral("configName")] = p.configName;
        obj[QStringLiteral("slots")]      = slotsObj;
        if (p.engine == 1) {
            obj[QStringLiteral("engine")]   = 1;
            obj[QStringLiteral("minColor")] = QJsonObject::fromVariantMap(p.minColor);
        }
        // Per-entry timestamps for the Manage modal (.e.4) — modified
        // refreshes on each write; created sticks to the file's
        // existing value if we can find it, else now.
        const QString iso = QDateTime::currentDateTimeUtc()
                                .toString(Qt::ISODate);
        obj[QStringLiteral("modified_at")] = iso;
        obj[QStringLiteral("created_at")]  = iso;
        arr.append(obj);
    }
    QJsonObject root;
    root[QStringLiteral("schema")]       = 1;
    root[QStringLiteral("user_presets")] = arr;

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("PresetManager: cannot open %s for write: %s",
                 qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        qWarning("PresetManager: commit failed for %s: %s",
                 qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    return true;
}

} // namespace qcv
