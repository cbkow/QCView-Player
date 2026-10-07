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

#include <QObject>
#include <QStringList>

namespace qcv {

class MinColorEngine : public QObject
{
    Q_OBJECT
    // Input (clip)
    Q_PROPERTY(int  inputGamut     READ inputGamut     WRITE setInputGamut     NOTIFY chainChanged)
    Q_PROPERTY(int  inputTransfer  READ inputTransfer  WRITE setInputTransfer  NOTIFY chainChanged)
    Q_PROPERTY(bool inputLimited   READ inputLimited   WRITE setInputLimited   NOTIFY chainChanged)
    // Knee (clip)
    Q_PROPERTY(bool   kneeEnabled    READ kneeEnabled    WRITE setKneeEnabled    NOTIFY chainChanged)
    Q_PROPERTY(double kneeSourceNits READ kneeSourceNits WRITE setKneeSourceNits NOTIFY chainChanged)
    Q_PROPERTY(double kneeTargetNits READ kneeTargetNits WRITE setKneeTargetNits NOTIFY chainChanged)
    Q_PROPERTY(double kneeStart      READ kneeStart      WRITE setKneeStart      NOTIFY chainChanged)
    // AgX (view)
    Q_PROPERTY(bool   agxEnabled    READ agxEnabled    WRITE setAgxEnabled    NOTIFY chainChanged)
    Q_PROPERTY(int    agxTarget     READ agxTarget     WRITE setAgxTarget     NOTIFY chainChanged)
    Q_PROPERTY(double agxPeak       READ agxPeak       WRITE setAgxPeak       NOTIFY chainChanged)
    Q_PROPERTY(double agxWhiteEv    READ agxWhiteEv    WRITE setAgxWhiteEv    NOTIFY chainChanged)
    Q_PROPERTY(double agxBlackEv    READ agxBlackEv    WRITE setAgxBlackEv    NOTIFY chainChanged)
    Q_PROPERTY(double agxContrast   READ agxContrast   WRITE setAgxContrast   NOTIFY chainChanged)
    Q_PROPERTY(double agxToe        READ agxToe        WRITE setAgxToe        NOTIFY chainChanged)
    Q_PROPERTY(double agxShoulder   READ agxShoulder   WRITE setAgxShoulder   NOTIFY chainChanged)
    Q_PROPERTY(double agxHueRestore READ agxHueRestore WRITE setAgxHueRestore NOTIFY chainChanged)
    Q_PROPERTY(double agxHdrPurity  READ agxHdrPurity  WRITE setAgxHdrPurity  NOTIFY chainChanged)
    // Rendering + Display (view)
    Q_PROPERTY(bool   openDrt    READ openDrt    WRITE setOpenDrt    NOTIFY chainChanged)
    Q_PROPERTY(int    look       READ look       WRITE setLook       NOTIFY chainChanged)
    Q_PROPERTY(int    tonescale  READ tonescale  WRITE setTonescale  NOTIFY chainChanged)
    Q_PROPERTY(int    creativeWhite READ creativeWhite WRITE setCreativeWhite NOTIFY chainChanged)
    Q_PROPERTY(double creativeWhiteLimit READ creativeWhiteLimit WRITE setCreativeWhiteLimit NOTIFY chainChanged)
    Q_PROPERTY(int    display    READ display    WRITE setDisplay    NOTIFY chainChanged)
    Q_PROPERTY(int    surround   READ surround   WRITE setSurround   NOTIFY chainChanged)
    Q_PROPERTY(double peakNits   READ peakNits   WRITE setPeakNits   NOTIFY chainChanged)
    Q_PROPERTY(double greyBoost  READ greyBoost  WRITE setGreyBoost  NOTIFY chainChanged)
    Q_PROPERTY(double hdrPurity  READ hdrPurity  WRITE setHdrPurity  NOTIFY chainChanged)
    Q_PROPERTY(double greyNits   READ greyNits   WRITE setGreyNits   NOTIFY chainChanged)
    // The reels' entries (vendored tables, by index).
    Q_PROPERTY(QStringList inputGamutNames    READ inputGamutNames    CONSTANT)
    Q_PROPERTY(QStringList inputTransferNames READ inputTransferNames CONSTANT)
    Q_PROPERTY(QStringList lookNames          READ lookNames          CONSTANT)
    Q_PROPERTY(QStringList tonescaleNames     READ tonescaleNames     CONSTANT)
    Q_PROPERTY(QStringList creativeWhiteNames READ creativeWhiteNames CONSTANT)
    Q_PROPERTY(QStringList displayNames       READ displayNames       CONSTANT)
    // The Display entry's EOTF kind, for the panel: 0 SDR power, 1 PQ,
    // 2 HLG, 3 linear hand-off. Dims entries that don't suit the mode.
    Q_PROPERTY(int displayKind READ displayKind NOTIFY chainChanged)

public:
    explicit MinColorEngine(QObject *parent = nullptr);

    const MinColorChain &chain() const { return m_chain; }
    void setChain(const MinColorChain &c);

    int  inputGamut() const    { return m_chain.input.gamut; }
    int  inputTransfer() const { return m_chain.input.transfer; }
    bool inputLimited() const  { return m_chain.input.limited; }
    void setInputGamut(int v);
    void setInputTransfer(int v);
    void setInputLimited(bool v);

    bool   kneeEnabled() const    { return m_chain.knee.enabled; }
    double kneeSourceNits() const { return m_chain.knee.sourceNits; }
    double kneeTargetNits() const { return m_chain.knee.targetNits; }
    double kneeStart() const      { return m_chain.knee.start; }
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
    // The EOTF kind of any Display entry (same codes), for dimming.
    Q_INVOKABLE int displayKindOf(int index) const;

signals:
    void chainChanged();

private:
    template <typename T> void set(T &field, const T &value);
    void persist() const;

    MinColorChain m_chain;
};

} // namespace qcv
