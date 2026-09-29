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

#include <optional>

#include "linear_stage.h"

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
};

struct OcioChainSpec {
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
        return !configPath.isEmpty() && !scene.input.isEmpty() && !display.isEmpty()
            && !view.isEmpty();
    }
    bool sameShader(const OcioChainSpec &o) const
    {
        return configPath == o.configPath && scene.sameShader(o.scene)
            && display == o.display && view == o.view && displayLutPath == o.displayLutPath
            && sdrDisplay == o.sdrDisplay && sdrView == o.sdrView;
    }
    bool operator==(const OcioChainSpec &o) const { return sameShader(o) && scene == o.scene; }
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
    OcioChainSpec single;   // single view: the clip on screen (else the default)
    OcioChainSpec a;        // dual view
    OcioChainSpec b;        //  … B is A's chain while ganged

    // Dual view needs a chain per side: the sides resolve differently.
    // Equal chains collapse to one pass over the canvas, as before.
    bool perSide() const { return dual && a != b; }
};

} // namespace qcv
