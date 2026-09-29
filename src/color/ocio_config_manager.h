// OCIOConfigManager — Phase 2.1.
//
// Owns the application-wide OpenColorIO config + the active
// [Input, View, Look, Display, Bypass] selection. All renderers and
// the slot-machine UI consume this singleton.
//
// Design contract per Guide 14:
// - Config defaults to OCIO 2.5's built-in ACES 2.0 CG config
//   (`ocio://default`). No bundled config files needed for v1.
// - Active-chain setters validate against the loaded config; an
//   invalid name is rejected with a warning, leaving the previous
//   value in place.
// - Bypass toggle short-circuits the entire chain via OCIO's
//   identity transform (Guide 15 §16 takeaway #9).
// - PIMPL: OCIO C++ headers do NOT leak to consumers. video
//   decoder, renderer, UI all see only Q_PROPERTY-friendly types.
//
// Colour plan stage 2 — per-clip scene chain. The scene side (Input,
// Look, Scene LUT / CDL, knee) is a default plus per-clip pins; the
// display side (Display, View, Display LUT) and the config are shared.
// The active* / knee* properties are the chain of the clip in FOCUS
// (single view: the clip on screen; dual view: the A or B tab). A
// clip-side edit always pins that clip — single and dual view
// read the same; with no clip loaded it sets the starting chain (the
// default every untouched clip shows).
// Renderers never read these: they take snapshot(), an immutable
// OcioChainSnapshot with one resolved OcioChainSpec per displayed side.

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <mutex>

#include "linear_stage.h"
#include "ocio_chain_spec.h"

namespace qcv {

class OCIOConfigManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString configDescription READ configDescription NOTIFY configChanged)
    Q_PROPERTY(QString configIdentifier READ configIdentifier NOTIFY configChanged)
    Q_PROPERTY(QStringList availableConfigs READ availableConfigs NOTIFY availableConfigsChanged)
    Q_PROPERTY(QString activeConfigName READ activeConfigName NOTIFY configChanged)
    Q_PROPERTY(QStringList colorspaces READ colorspaces NOTIFY configChanged)
    Q_PROPERTY(QStringList displays READ displays NOTIFY configChanged)
    Q_PROPERTY(QStringList looks READ looks NOTIFY configChanged)
    Q_PROPERTY(QString activeInput READ activeInput WRITE setActiveInput NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeDisplay READ activeDisplay WRITE setActiveDisplay NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeView READ activeView WRITE setActiveView NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeLook READ activeLook WRITE setActiveLook NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeSceneLutPath READ activeSceneLutPath WRITE setActiveSceneLutPath NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeDisplayLutPath READ activeDisplayLutPath WRITE setActiveDisplayLutPath NOTIFY activeChainChanged)
    // CDL collections (.ccc / .cdl) in the Scene LUT slot: which
    // correction — its id, or an index; empty = the first. Cleared when
    // the Scene LUT path changes.
    Q_PROPERTY(QString activeSceneLutCccId READ activeSceneLutCccId WRITE setActiveSceneLutCccId NOTIFY activeChainChanged)
    // OCIO is something the user deliberately *engages* — default
    // off, so the app boots showing the raw image. Slot-machine
    // selections persist on the manager regardless of engagement;
    // flipping engaged ON is the explicit "apply this chain" action,
    // and once engaged subsequent slot edits live-update.
    Q_PROPERTY(bool engaged READ engaged WRITE setEngaged NOTIFY activeChainChanged)
    // Highlight Knee — the chain step between Scene LUT and Output (see
    // linear_stage.h). Changing it never bumps activeChainGeneration: the
    // renderers key their pipeline on (generation, stage identity) and
    // read the parameters per frame via linearStageSettings().
    Q_PROPERTY(bool   kneeEnabled    READ kneeEnabled    WRITE setKneeEnabled    NOTIFY kneeChanged)
    Q_PROPERTY(double kneeSourceNits READ kneeSourceNits WRITE setKneeSourceNits NOTIFY kneeChanged)
    Q_PROPERTY(double kneeTargetNits READ kneeTargetNits WRITE setKneeTargetNits NOTIFY kneeChanged)
    // Fraction of the target peak (PQ-normalized) where the shoulder
    // starts; < 0 = BT.2390's default for the current peaks.
    Q_PROPERTY(double kneeStart      READ kneeStart      WRITE setKneeStart      NOTIFY kneeChanged)
    // Resolved for the active chain: can it split (interchange roles,
    // not a data colourspace / view), and is the Display/View SDR (the
    // knee target is then 100 nits, not kneeTargetNits).
    Q_PROPERTY(bool   kneeAvailable  READ kneeAvailable  NOTIFY activeChainChanged)
    Q_PROPERTY(bool   displayIsSdr   READ displayIsSdr   NOTIFY activeChainChanged)
    // Knee start in nits for the current settings, for the UI read-out.
    Q_PROPERTY(double kneeStartNits  READ kneeStartNits  NOTIFY kneeChanged)
    // The knee start actually in use, as a fraction (BT.2390's value when
    // kneeStart < 0) — the slider's position.
    Q_PROPERTY(double kneeStartEffective READ kneeStartEffective NOTIFY kneeChanged)

    // Per-clip scene chain. Which scene-side slots of the focused clip
    // are pinned to it (else they follow the default).
    Q_PROPERTY(bool inputPinned    READ inputPinned    NOTIFY activeChainChanged)
    Q_PROPERTY(bool lookPinned     READ lookPinned     NOTIFY activeChainChanged)
    Q_PROPERTY(bool sceneLutPinned READ sceneLutPinned NOTIFY activeChainChanged)
    Q_PROPERTY(bool kneePinned     READ kneePinned     NOTIFY activeChainChanged)
    // The clip whose chain the panel shows; empty = none (the default).
    Q_PROPERTY(QString focusClipId READ focusClipId NOTIFY viewContextChanged)
    Q_PROPERTY(bool    dualView    READ dualView    NOTIFY viewContextChanged)
    Q_PROPERTY(QString clipIdA     READ clipIdA     NOTIFY viewContextChanged)
    Q_PROPERTY(QString clipIdB     READ clipIdB     NOTIFY viewContextChanged)
    // Bumps whenever any clip's own chain changes (or the config does) —
    // QML binds badges through it: `ocio.pinsRevision, ocio.clipBadge(id)`.
    Q_PROPERTY(int pinsRevision READ pinsRevision NOTIFY pinsRevisionChanged)
    // Dual view: which side's scene chain the panel edits (0 = A, 1 = B).
    Q_PROPERTY(int activeTab READ activeTab WRITE setActiveTab NOTIFY viewContextChanged)

public:
    explicit OCIOConfigManager(QObject *parent = nullptr);
    ~OCIOConfigManager() override;

    // QML-callable.
    Q_INVOKABLE bool loadBuiltInDefault();
    Q_INVOKABLE bool loadConfigFile(const QString &path);
    // A bundled config's file (assets/OCIO/<dirName>/config.ocio), or ""
    // when it isn't shipped. The scopes read files the live config can't
    // name through Blender 5.2 (ScopeController).
    static QString bundledConfigPath(const QString &dirName);

    // Switch to one of the enumerated configs. `name` must be a
    // member of availableConfigs(). Resets the active chain to the
    // new config's defaults (a chain valid for one config is rarely
    // valid for another). No-op if name is unknown.
    Q_INVOKABLE bool setActiveConfig(const QString &name);

    // Read-only views of the loaded config.
    QString configDescription() const;
    QString configIdentifier() const;     // "ocio://default" or file path
    QStringList availableConfigs() const;
    QString activeConfigName() const   { return m_activeConfigName; }
    QStringList colorspaces() const;
    QStringList displays() const;
    QStringList looks() const;
    Q_INVOKABLE QStringList viewsForDisplay(const QString &display) const;

    // Bake the active OCIO chain (Look → Scene LUT → DisplayView →
    // Display LUT) to a .cube 3D LUT file at `outPath`. cubeSize is
    // the per-axis grid resolution (65 = 65³ samples ≈ 274K points,
    // sub-100 ms on M-series). Returns empty string on success;
    // human-readable error message on failure.
    //
    // Independent of `engaged` — the chain just needs to be
    // *configured* (input + display + view all set). Synchronous;
    // safe to call from the QML thread at the default 65³ size.
    Q_INVOKABLE QString exportLut(const QString &outPath, int cubeSize = 65);

    // SDR sRGB equivalent of the active Display/View, for captures
    // (screenshots, note thumbnails, modal backdrop). Captures are
    // 8-bit sRGB files, but the live chain may target a linear EDR,
    // PQ/HLG or P3 display whose output is wrong stored as sRGB.
    // Resolves the config's sRGB display plus the closest matching
    // view ("ACES 2.0 - HDR 1000 nits" → "ACES 2.0", "... (P3 D65)" →
    // "ACES 2.0 - SDR 100 nits (Rec.709)", "Standard (No Tonemap)" →
    // "Standard"). Returns false when no override is needed (already
    // on the sRGB display) or the config has no sRGB display.
    bool sdrCaptureDisplayView(QString *display, QString *view) const;

    // The focused clip's chain (scene side resolved; display side shared).
    QString activeInput()           const { return focusedScene().input; }
    QString activeDisplay()         const { return m_activeDisplay; }
    QString activeView()            const { return m_activeView; }
    QString activeLook()            const { return focusedScene().look; }
    QString activeSceneLutPath()    const { return focusedScene().sceneLutPath; }
    QString activeDisplayLutPath()  const { return m_activeDisplayLutPath; }
    QString activeSceneLutCccId()   const { return focusedScene().sceneLutCccId; }
    void    setActiveSceneLutCccId(const QString &id);
    bool    engaged()               const { return m_engaged; }

    void setActiveInput(const QString &name);
    void setActiveDisplay(const QString &name);
    void setActiveView(const QString &name);
    void setActiveLook(const QString &name);
    // LUT-slot path setters (Phase 2.5). Empty path clears the slot.
    // Non-empty path is validated for existence + supported extension
    // (.cube / .3dl / .csp); rejection leaves previous value.
    void setActiveSceneLutPath(const QString &path);
    void setActiveDisplayLutPath(const QString &path);
    void setEngaged(bool b);

    // Monotonically-increasing counter incremented each time the
    // active chain changes. The render thread polls this in
    // synchronize() and rebuilds the OCIO pipeline when it bumps.
    // Atomic for cross-thread reads.
    int activeChainGeneration() const {
        return m_activeChainGeneration.load(std::memory_order_acquire);
    }

    bool   kneeEnabled()    const { return focusedScene().kneeEnabled; }
    double kneeSourceNits() const { return focusedScene().kneeSourceNits; }
    double kneeTargetNits() const { return focusedScene().kneeTargetNits; }
    double kneeStart()      const { return focusedScene().kneeStart; }
    void   setKneeEnabled(bool on);
    void   setKneeSourceNits(double nits);
    void   setKneeTargetNits(double nits);
    void   setKneeStart(double fraction);
    bool   kneeAvailable() const;
    bool   displayIsSdr() const;
    double kneeStartNits() const;
    double kneeStartEffective() const;

    // The focused clip's stage settings; `gain` is the Brightness. GUI
    // thread (renderers use snapshot()).
    LinearStageSettings linearStageSettings(float gain) const;

    // Bumps on every knee change — the D3D11 render-on-demand loop polls
    // it (with activeChainGeneration) to know it must redraw.
    int stageGeneration() const {
        return m_stageGeneration.load(std::memory_order_acquire);
    }

    // ---- Per-clip scene chain ----
    enum class Slot { Input, Look, SceneLut, Knee };

    // What is on screen, from WindowManager: the single-view clip (the
    // playlist clip under the playhead in a playlist), and the dual
    // sides. Empty ids = nothing loaded (the default chain).
    void setViewContext(const QString &singleClipId, bool dual,
                        const QString &clipA, const QString &clipB);

    QString focusClipId() const;
    bool    dualView()    const { return m_dual; }
    QString clipIdA()     const { return m_clipA; }
    QString clipIdB()     const { return m_clipB; }
    int     activeTab()   const { return m_activeTab; }
    void    setActiveTab(int tab);

    bool inputPinned()    const { return slotPinned(Slot::Input); }
    bool lookPinned()     const { return slotPinned(Slot::Look); }
    bool sceneLutPinned() const { return slotPinned(Slot::SceneLut); }
    bool kneePinned()     const { return slotPinned(Slot::Knee); }

    // Pin the focused clip's current value of `slot` ("input", "look",
    // "sceneLut", "knee") to it, or (pinned = false) return that slot to
    // the default — the panel's ↺.
    Q_INVOKABLE void setSlotPinned(const QString &slot, bool pinned);
    // Dual view: B's pins become A's effective scene chain. Not in the
    // panel (kept for the bulk actions, colour plan stage 3).
    Q_INVOKABLE void copyAChainToB();
    Q_INVOKABLE bool clipHasPins(const QString &clipId) const;
    Q_INVOKABLE void clearClipPins(const QString &clipId);

    // Bulk (project panel, multi-select): set the Input of several clips;
    // give them `fromClipId`'s whole clip chain; reset them to the
    // default. One publish each.
    Q_INVOKABLE void setInputForClips(const QStringList &clipIds, const QString &colourspace);
    Q_INVOKABLE void copyClipChain(const QString &fromClipId, const QStringList &toClipIds);
    Q_INVOKABLE void resetClipChains(const QStringList &clipIds);
    OcioScenePin clipPins(const QString &clipId) const { return m_pins.value(clipId); }
    // Persistence (MediaItem::ocioClip): one clip's pins, and every
    // clip's at once when a project loads (replaces all, one publish).
    QVariantMap clipPinsVariant(const QString &clipId) const
    {
        return m_pins.value(clipId).toVariant();
    }
    void replaceAllPins(const QHash<QString, QVariantMap> &pins);

    // Badges for clips with their own chain (project panel, A/B chips):
    // the Input's short name (colourspace_short_name.h) plus what else
    // the clip sets ("Lin Rec.709 + Look"); empty for an untouched clip.
    // The tooltip lists each set slot with its exact name.
    int pinsRevision() const { return m_pinsRevision; }
    Q_INVOKABLE QString clipBadge(const QString &clipId) const;
    Q_INVOKABLE QString clipBadgeTooltip(const QString &clipId) const;

    // Resolved chains. focusedSpec / specForClip: GUI thread.
    OcioChainSpec focusedSpec() const;
    OcioChainSpec specForClip(const QString &clipId) const;
    // Render threads: what to draw with. Replaced whole on every change.
    std::shared_ptr<const OcioChainSnapshot> snapshot() const;

signals:
    void configChanged();
    void activeChainChanged();
    void availableConfigsChanged();
    void kneeChanged();
    void viewContextChanged();
    // A clip's pins changed (persistence, badges).
    void pinsChanged(const QString &clipId);
    // Every clip's pins were replaced (a project loaded).
    void pinsReloaded();
    void pinsRevisionChanged();

private:
    void resetActiveDefaults();
    // Rebuild the snapshot, bump the generation and notify — after any
    // change to the default, a pin, the display side or the context.
    void publish(bool knee = false);
    OcioSceneChain resolveScene(const QString &clipId) const;
    OcioSceneChain focusedScene() const { return resolveScene(focusClipId()); }
    OcioChainSpec  specFor(const OcioSceneChain &scene) const;
    bool slotPinned(Slot slot) const;
    // Where an edit of `slot` lands: the focused clip's pin (created on
    // first edit), or nullptr for the default when no clip is loaded.
    OcioScenePin *editPin(Slot slot);
    void notePinEdit(const QString &clipId);
    // Knee edits start from the focused clip's effective knee.
    template <typename Fn> void editKnee(Fn &&fn);
    bool isValidColorSpace(const QString &name) const;
    bool isValidDisplay(const QString &name) const;
    bool isValidViewForDisplay(const QString &display, const QString &view) const;
    bool isValidLook(const QString &name) const;

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // Phase 2.5c: enumerated set of configs the user can switch
    // between. Built once at startup from $OCIO env var + scan of
    // assets/OCIO/. Display name → absolute config path. Order
    // matters (first entry is the priority pick on startup) so
    // QHash isn't right; QList of pairs.
    struct ConfigSlot {
        QString displayName;     // "Blender 5.1", "$OCIO", etc.
        QString path;            // absolute path to config.ocio
    };
    QList<ConfigSlot> m_configSlots;
    QString m_activeConfigName;

    void enumerateConfigs();

    QString m_configIdentifier;
    OcioSceneChain m_default;      // the scene side of every unpinned slot
    QString m_activeDisplay;
    QString m_activeView;
    QString m_activeDisplayLutPath;
    bool    m_engaged = false;     // default disengaged — see Q_PROPERTY note
    std::atomic<int> m_activeChainGeneration{0};
    std::atomic<int> m_stageGeneration{0};

    QHash<QString, OcioScenePin> m_pins;   // media item id → its pins
    int m_pinsRevision = 0;
    void bumpPinsRevision() { ++m_pinsRevision; emit pinsRevisionChanged(); }
    // Short Input names for the loaded config (built on first use).
    mutable QHash<QString, QString> m_shortNames;
    mutable QString                 m_shortNamesConfig;
    QString m_singleClip;
    QString m_clipA, m_clipB;
    bool    m_dual      = false;
    int     m_activeTab = 0;

    mutable std::mutex m_snapshotMutex;
    std::shared_ptr<const OcioChainSnapshot> m_snapshot;
};

} // namespace qcv
