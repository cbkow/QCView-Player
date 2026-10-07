// MinColorEngine — the minColor engine's state, as QML sees it.
//
// Holds one MinColorChain (mincolor_chain.h): the Input and Knee the panel
// edits for the clip half, the AgX and Output of the view half. Every
// setter below is a uniform update downstream; nothing here compiles.
// Persisted in QSettings as a whole (the panel's last state, like the OCIO
// engine's slot selections); per-clip pins and project persistence come
// with the carry-over step.
//
// Engagement and engine selection are not here: OCIOConfigManager owns the
// one On / Off switch and the OCIO | minColor selector, and publishes the
// snapshot both engines ride on. It reads chain() when it publishes and
// listens to chainChanged().

#pragma once

#include "mincolor_chain.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace qcv {

class MinColorEngine : public QObject
{
    Q_OBJECT
    // Input (clip)
    Q_PROPERTY(int  inputGamut     READ inputGamut     WRITE setInputGamut     NOTIFY inputChanged)
    Q_PROPERTY(int  inputTransfer  READ inputTransfer  WRITE setInputTransfer  NOTIFY inputChanged)
    Q_PROPERTY(bool inputLimited   READ inputLimited   WRITE setInputLimited   NOTIFY inputChanged)
    // Knee (clip)
    Q_PROPERTY(bool   kneeEnabled    READ kneeEnabled    WRITE setKneeEnabled    NOTIFY kneeChanged)
    Q_PROPERTY(double kneeSourceNits READ kneeSourceNits WRITE setKneeSourceNits NOTIFY kneeChanged)
    Q_PROPERTY(double kneeTargetNits READ kneeTargetNits WRITE setKneeTargetNits NOTIFY kneeChanged)
    Q_PROPERTY(double kneeStart      READ kneeStart      WRITE setKneeStart      NOTIFY kneeChanged)
    // AgX (view)
    Q_PROPERTY(bool   agxEnabled    READ agxEnabled    WRITE setAgxEnabled    NOTIFY agxChanged)
    Q_PROPERTY(int    agxTarget     READ agxTarget     WRITE setAgxTarget     NOTIFY agxChanged)
    Q_PROPERTY(double agxPeak       READ agxPeak       WRITE setAgxPeak       NOTIFY agxChanged)
    Q_PROPERTY(double agxWhiteEv    READ agxWhiteEv    WRITE setAgxWhiteEv    NOTIFY agxChanged)
    Q_PROPERTY(double agxBlackEv    READ agxBlackEv    WRITE setAgxBlackEv    NOTIFY agxChanged)
    Q_PROPERTY(double agxContrast   READ agxContrast   WRITE setAgxContrast   NOTIFY agxChanged)
    Q_PROPERTY(double agxToe        READ agxToe        WRITE setAgxToe        NOTIFY agxChanged)
    Q_PROPERTY(double agxShoulder   READ agxShoulder   WRITE setAgxShoulder   NOTIFY agxChanged)
    Q_PROPERTY(double agxHueRestore READ agxHueRestore WRITE setAgxHueRestore NOTIFY agxChanged)
    Q_PROPERTY(double agxHdrPurity  READ agxHdrPurity  WRITE setAgxHdrPurity  NOTIFY agxChanged)
    // Rendering + Display (view)
    Q_PROPERTY(bool   openDrt    READ openDrt    WRITE setOpenDrt    NOTIFY outputChanged)
    Q_PROPERTY(int    look       READ look       WRITE setLook       NOTIFY outputChanged)
    Q_PROPERTY(int    tonescale  READ tonescale  WRITE setTonescale  NOTIFY outputChanged)
    Q_PROPERTY(int    creativeWhite READ creativeWhite WRITE setCreativeWhite NOTIFY outputChanged)
    Q_PROPERTY(double creativeWhiteLimit READ creativeWhiteLimit WRITE setCreativeWhiteLimit NOTIFY outputChanged)
    Q_PROPERTY(int    display    READ display    WRITE setDisplay    NOTIFY outputChanged)
    Q_PROPERTY(int    surround   READ surround   WRITE setSurround   NOTIFY outputChanged)
    Q_PROPERTY(double peakNits   READ peakNits   WRITE setPeakNits   NOTIFY outputChanged)
    Q_PROPERTY(double greyBoost  READ greyBoost  WRITE setGreyBoost  NOTIFY outputChanged)
    Q_PROPERTY(double hdrPurity  READ hdrPurity  WRITE setHdrPurity  NOTIFY outputChanged)
    Q_PROPERTY(double greyNits   READ greyNits   WRITE setGreyNits   NOTIFY outputChanged)
    // What the shared KneeColumn reads off its `ocio` object (the OCIO
    // manager has the same names): the knee is always available here, is
    // never pinned until per-clip pins land, and targets 100 nits when
    // the Display encoding is an SDR power curve.
    Q_PROPERTY(bool    kneeAvailable      READ kneeAvailable      CONSTANT)
    // Per-clip pins (the Input and Knee the focused clip keeps): the
    // panel's ↺ and ClipSetDot read these; `pinsRevision` bumps on any
    // pin change so badges re-read.
    Q_PROPERTY(bool    inputPinned        READ inputPinned        NOTIFY inputChanged)
    Q_PROPERTY(bool    kneePinned         READ kneePinned         NOTIFY kneeChanged)
    Q_PROPERTY(QString focusClipId        READ focusClipId        NOTIFY focusClipChanged)
    Q_PROPERTY(int     pinsRevision       READ pinsRevision       NOTIFY pinsRevisionChanged)
    Q_PROPERTY(bool    displayIsSdr       READ displayIsSdr       NOTIFY outputChanged)
    Q_PROPERTY(double  kneeStartEffective READ kneeStartEffective NOTIFY kneeChanged)
    Q_PROPERTY(double  kneeStartNits      READ kneeStartNits      NOTIFY kneeChanged)

    // The reels' entries (vendored tables, by index).
    Q_PROPERTY(QStringList inputGamutNames    READ inputGamutNames    CONSTANT)
    Q_PROPERTY(QStringList inputTransferNames READ inputTransferNames CONSTANT)
    Q_PROPERTY(QStringList lookNames          READ lookNames          CONSTANT)
    Q_PROPERTY(QStringList tonescaleNames     READ tonescaleNames     CONSTANT)
    Q_PROPERTY(QStringList creativeWhiteNames READ creativeWhiteNames CONSTANT)
    Q_PROPERTY(QStringList displayNames       READ displayNames       CONSTANT)
    // The Display entry's EOTF kind, for the panel: 0 SDR power, 1 PQ,
    // 2 HLG, 3 linear hand-off. Dims entries that don't suit the mode.
    Q_PROPERTY(int displayKind READ displayKind NOTIFY outputChanged)

public:
    explicit MinColorEngine(QObject *parent = nullptr);

    // The default chain (every unpinned slot) — what the panel edits when
    // no clip is focused, and what presets capture and apply.
    const MinColorChain &chain() const { return m_chain; }
    void setChain(const MinColorChain &c);
    // The chain a clip renders with: the default with its pins applied.
    MinColorChain resolveFor(const QString &clipId) const;
    // What the panel shows and edits: the focused clip's chain.
    MinColorChain focused() const { return resolveFor(focusClipId()); }

    // The clip whose chain the panel edits — OCIOConfigManager owns the
    // view context (single clip, dual A / B, the active tab) and hands
    // it over through this. Empty = the default.
    void setFocusClipFn(std::function<QString()> fn) { m_focusFn = std::move(fn); }
    // Pins, mirrored to the project by WindowManager (pinsChanged) and
    // loaded back on project open.
    void setSlotPinned(const QString &slot, bool pinned);   // "mcInput" / "mcKnee"
    QVariantMap clipPinsVariant(const QString &clipId) const;
    void replaceAllPins(const QHash<QString, QVariantMap> &pins);
    QString clipBadge(const QString &clipId) const;
    QString clipBadgeTooltip(const QString &clipId) const;
    int  pinsRevision() const { return m_pinsRevision; }
    // The swapchain is display-linear (macOS EDR): a linear Display
    // hand-off is scaled by peak / 100 (mincolor::resolve).
    void setEdrLinear(bool on);
    bool edrLinear() const { return m_edrLinear; }

    int  inputGamut() const    { return focused().input.gamut; }
    int  inputTransfer() const { return focused().input.transfer; }
    bool inputLimited() const  { return focused().input.limited; }
    void setInputGamut(int v);
    void setInputTransfer(int v);
    void setInputLimited(bool v);

    bool   kneeEnabled() const    { return focused().knee.enabled; }
    double kneeSourceNits() const { return focused().knee.sourceNits; }
    double kneeTargetNits() const { return focused().knee.targetNits; }
    double kneeStart() const      { return focused().knee.start; }
    void setKneeEnabled(bool v);
    void setKneeSourceNits(double v);
    void setKneeTargetNits(double v);
    void setKneeStart(double v);

    bool   agxEnabled() const    { return m_chain.agx.enabled; }
    int    agxTarget() const     { return m_chain.agx.target; }
    double agxPeak() const       { return m_chain.agx.peak; }
    double agxWhiteEv() const    { return m_chain.agx.whiteEv; }
    double agxBlackEv() const    { return m_chain.agx.blackEv; }
    double agxContrast() const   { return m_chain.agx.contrast; }
    double agxToe() const        { return m_chain.agx.toe; }
    double agxShoulder() const   { return m_chain.agx.shoulder; }
    double agxHueRestore() const { return m_chain.agx.hueRestore; }
    double agxHdrPurity() const  { return m_chain.agx.hdrPurity; }
    void setAgxEnabled(bool v);
    void setAgxTarget(int v);
    void setAgxPeak(double v);
    void setAgxWhiteEv(double v);
    void setAgxBlackEv(double v);
    void setAgxContrast(double v);
    void setAgxToe(double v);
    void setAgxShoulder(double v);
    void setAgxHueRestore(double v);
    void setAgxHdrPurity(double v);

    bool   openDrt() const       { return m_chain.output.openDrt; }
    int    look() const          { return m_chain.output.look; }
    int    tonescale() const     { return m_chain.output.tonescale; }
    int    creativeWhite() const { return m_chain.output.cwp; }
    double creativeWhiteLimit() const { return m_chain.output.cwpLimit; }
    int    display() const       { return m_chain.output.display; }
    int    surround() const      { return m_chain.output.surround; }
    double peakNits() const      { return m_chain.output.peakNits; }
    double greyBoost() const     { return m_chain.output.greyBoost; }
    double hdrPurity() const     { return m_chain.output.hdrPurity; }
    double greyNits() const      { return m_chain.output.greyNits; }
    void setOpenDrt(bool v);
    void setLook(int v);
    void setTonescale(int v);
    void setCreativeWhite(int v);
    void setCreativeWhiteLimit(double v);
    void setDisplay(int v);
    void setSurround(int v);
    void setPeakNits(double v);
    void setGreyBoost(double v);
    void setHdrPurity(double v);
    void setGreyNits(double v);

    QStringList inputGamutNames() const    { return mincolor::inputGamutNames(); }
    QStringList inputTransferNames() const { return mincolor::inputTransferNames(); }
    QStringList lookNames() const          { return mincolor::lookNames(); }
    QStringList tonescaleNames() const     { return mincolor::tonescaleNames(); }
    QStringList creativeWhiteNames() const { return mincolor::creativeWhiteNames(); }
    QStringList displayNames() const       { return mincolor::displayNames(); }
    int displayKind() const;
    bool    kneeAvailable() const { return true; }
    bool    inputPinned() const;
    bool    kneePinned() const;
    QString focusClipId() const   { return m_focusFn ? m_focusFn() : QString(); }
    bool    displayIsSdr() const  { return displayKind() == 0; }
    double  kneeStartEffective() const;
    double  kneeStartNits() const;
    // The EOTF kind of any Display entry (same codes), for dimming.
    Q_INVOKABLE int displayKindOf(int index) const;

signals:
    // Per group, so a knee drag re-evaluates knee bindings only (the
    // panel's reels bind to their own group); chainChanged is the
    // aggregate the OCIO manager republishes on.
    void inputChanged();
    void kneeChanged();
    void focusClipChanged();   // the view context moved (OCIOConfigManager)
    void agxChanged();
    void outputChanged();
    void chainChanged();
    void pinsChanged(const QString &clipId);
    void pinsRevisionChanged();

private:
    using GroupSignal = void (MinColorEngine::*)();
    template <typename T> void set(T &field, const T &value, GroupSignal group);
    // Clip-side fields: edit the focused clip's pin when one is focused,
    // else the default.
    template <typename T> void setInputField(T MinColorInput::*field, const T &value);
    template <typename T> void setKneeField(T MinColorKnee::*field, const T &value);
    void persist();          // schedules the QSettings write (debounced)
    void notePinEdit(const QString &clipId, GroupSignal group);
    void emitGroup(GroupSignal group);

    MinColorChain m_chain;
    QHash<QString, MinColorPin> m_pins;   // media item id → its pins
    std::function<QString()> m_focusFn;
    int  m_pinsRevision = 0;
    bool m_edrLinear = false;
    QTimer m_persistTimer;
};

} // namespace qcv
