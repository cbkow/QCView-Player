// OcioChainSpec — one fully resolved OCIO chain, as a value.
//
// Colour plan stage 2: the scene side of the chain (Input, Look, Scene
// LUT / CDL, knee) belongs to the clip — a per-clip pin, else the default —
// while the display side (Display, View, Display LUT) and the config are
// shared. OCIOConfigManager resolves those into one spec per displayed
// side and publishes them as an immutable OcioChainSnapshot, so renderers
// and background compiles never read the manager's live fields.
//
// Knee parameters are per-frame uniforms: two specs that differ only in
// them share a compiled shader (sameShader()).

#pragma once

#include <QString>
#include <QVariantMap>

#include <optional>
#include <vector>

#include "linear_stage.h"
#include "mincolor_chain.h"

namespace qcv {

// The scene side: what describes the image.
struct OcioSceneChain {
    QString input;
    QString look;
    QString sceneLutPath;
    QString sceneLutCccId;   // CDL collections: which correction (empty = first)

    bool  kneeEnabled    = false;
    float kneeSourceNits = 1000.0f;
    float kneeTargetNits = 1000.0f;
    float kneeStart      = -1.0f;   // < 0 = BT.2390's default

    bool sameShader(const OcioSceneChain &o) const
    {
        return input == o.input && look == o.look && sceneLutPath == o.sceneLutPath
            && sceneLutCccId == o.sceneLutCccId;
    }
    bool operator==(const OcioSceneChain &o) const
    {
        return sameShader(o) && kneeEnabled == o.kneeEnabled
            && kneeSourceNits == o.kneeSourceNits && kneeTargetNits == o.kneeTargetNits
            && kneeStart == o.kneeStart;
    }
    bool operator!=(const OcioSceneChain &o) const { return !(*this == o); }
};

// A clip's pins: each scene-side slot either follows the default or is
// pinned to the clip. The knee is one slot (enabled + its three values).
struct OcioScenePin {
    struct SceneLut { QString path, cccId; };
    struct Knee { bool enabled = false; float sourceNits = 1000, targetNits = 1000, start = -1; };

    std::optional<QString>  input;
    std::optional<QString>  look;
    std::optional<SceneLut> sceneLut;
    std::optional<Knee>     knee;

    bool empty() const { return !input && !look && !sceneLut && !knee; }

    // The project file's form (MediaItem::ocioClip): only set slots.
    QVariantMap toVariant() const
    {
        QVariantMap m;
        if (input) m.insert(QStringLiteral("input"), *input);
        if (look)  m.insert(QStringLiteral("look"), *look);
        if (sceneLut) {
            m.insert(QStringLiteral("sceneLut"), QVariantMap{
                {QStringLiteral("path"), sceneLut->path},
                {QStringLiteral("cccId"), sceneLut->cccId}});
        }
        if (knee) {
            m.insert(QStringLiteral("knee"), QVariantMap{
                {QStringLiteral("enabled"), knee->enabled},
                {QStringLiteral("sourceNits"), double(knee->sourceNits)},
                {QStringLiteral("targetNits"), double(knee->targetNits)},
                {QStringLiteral("start"), double(knee->start)}});
        }
        return m;
    }
    static OcioScenePin fromVariant(const QVariantMap &m)
    {
        OcioScenePin p;
        if (m.contains(QStringLiteral("input"))) p.input = m.value(QStringLiteral("input")).toString();
        if (m.contains(QStringLiteral("look")))  p.look  = m.value(QStringLiteral("look")).toString();
        if (m.contains(QStringLiteral("sceneLut"))) {
            const QVariantMap l = m.value(QStringLiteral("sceneLut")).toMap();
            p.sceneLut = SceneLut{l.value(QStringLiteral("path")).toString(),
                                  l.value(QStringLiteral("cccId")).toString()};
        }
        if (m.contains(QStringLiteral("knee"))) {
            const QVariantMap k = m.value(QStringLiteral("knee")).toMap();
            p.knee = Knee{k.value(QStringLiteral("enabled")).toBool(),
                          float(k.value(QStringLiteral("sourceNits"), 1000.0).toDouble()),
                          float(k.value(QStringLiteral("targetNits"), 1000.0).toDouble()),
                          float(k.value(QStringLiteral("start"), -1.0).toDouble())};
        }
        return p;
    }
};

// Which engine a spec draws with. OCIO is the chain below; minColor is
// the OCIO-free engine (mincolor_chain.h) carried as resolved GPU blocks.
// The one On / Off switch (OcioChainSnapshot::engaged) bypasses both.
enum class ColorEngine : int { Ocio = 0, MinColor = 1 };

struct OcioChainSpec {
    ColorEngine    engine = ColorEngine::Ocio;
    // minColor engine: the side's resolved blocks (every change is a
    // uniform update on one kernel; nothing else below applies).
    MinColorGpu    minColor;

    QString        configPath;   // OCIOConfigManager::configIdentifier()
    OcioSceneChain scene;
    QString        display;
    QString        view;
    QString        displayLutPath;
    // SDR sRGB equivalent of display / view for captures
    // (OCIOConfigManager::sdrCaptureDisplayView); empty = no override.
    QString        sdrDisplay;
    QString        sdrView;

    bool complete() const
    {
        if (engine == ColorEngine::MinColor) return true;   // one kernel, no config
        return !configPath.isEmpty() && !scene.input.isEmpty() && !display.isEmpty()
            && !view.isEmpty();
    }
    // Same compiled shader. minColor specs always share theirs: the
    // kernel is fixed and the blocks are uniforms.
    bool sameShader(const OcioChainSpec &o) const
    {
        if (engine != o.engine) return false;
        if (engine == ColorEngine::MinColor) return true;
        return configPath == o.configPath && scene.sameShader(o.scene)
            && display == o.display && view == o.view && displayLutPath == o.displayLutPath
            && sdrDisplay == o.sdrDisplay && sdrView == o.sdrView;
    }
    bool operator==(const OcioChainSpec &o) const
    {
        if (!sameShader(o)) return false;
        if (engine == ColorEngine::MinColor) return minColor == o.minColor;
        return scene == o.scene;
    }
    bool operator!=(const OcioChainSpec &o) const { return !(*this == o); }

    // The linear stage for this chain; `gain` is the viewer's Brightness.
    LinearStageSettings stage(float gain) const
    {
        LinearStageSettings s;
        s.gain           = gain;
        s.kneeEnabled    = scene.kneeEnabled;
        s.kneeSourceNits = scene.kneeSourceNits;
        s.kneeTargetNits = scene.kneeTargetNits;
        s.kneeStart      = scene.kneeStart;
        return s;
    }
};

// What the renderers draw with, published as a whole on every change.
struct OcioChainSnapshot {
    int  generation = 0;
    bool engaged    = false;
    bool dual       = false;
    ColorEngine engine = ColorEngine::Ocio;   // what `engaged` runs; the specs carry it too
    OcioChainSpec single;   // single view: the clip on screen (else the default)
    OcioChainSpec a;        // dual view
    OcioChainSpec b;
    // Every distinct chain in the project (the default plus each clip's
    // own), for the renderers to build ahead of time: a playlist cut or a
    // clip switch then swaps to a ready pipeline instead of drawing the
    // new clip through the old chain while it compiles.
    std::vector<OcioChainSpec> warm;

    // Dual view needs a chain per side: the sides resolve differently.
    // Equal chains collapse to one pass over the canvas, as before.
    bool perSide() const { return dual && a != b; }
};

} // namespace qcv
