// ScopeController — the vectorscope's GUI side (WindowManager.scope).
//
// Resolves which colour space the scope interprets the source in, as a
// labelled tier (color/scope_math.h):
//   Input    — OCIO engaged: the user's Input. A note appears when the
//              file's own tags point somewhere else.
//   Assumed  — OCIO off: from the file's tags / format rules (untagged or
//              BT.709 video → Rec.1886, PQ + 2020 → Rec.2100-PQ, HLG,
//              P3 + PQ → ST2084-P3-D65, EXR → Linear Rec.709, stills →
//              sRGB), resolved to a colourspace of the live config.
//   Signal   — nothing resolvable: Y'CbCr straight from the source.
// Scale: SDR (Rec.709 / BT.1886) for an sdr-video colourspace, HDR (PQ
// Rec.2020) for everything else.
// Dual view: with OCIO off each side resolves its own tier from its own
// file (an SDR B beside a PQ A); with OCIO engaged both use the Input,
// like the picture's single chain. The scale is shared — HDR when either
// side needs it.
//
// Pushes the resulting ScopeConfig to the renderer while the scope panel
// is visible (setActive from QML), polls finished images into the
// "qcvscope" image provider, and publishes the graticule geometry
// (targets, gamut hexagons, skin line) in scope coordinates 0..1.

#pragma once

#include "color/scope_math.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <functional>

namespace qcv {

class BackdropImageProvider;
class IPlayerRenderer;
class OCIOConfigManager;
class ProjectManager;

class ScopeController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    active      READ active      WRITE setActive      NOTIFY activeChanged)
    Q_PROPERTY(int     zoom        READ zoom        WRITE setZoom        NOTIFY optionsChanged)
    Q_PROPERTY(bool    colorize    READ colorize    WRITE setColorize    NOTIFY optionsChanged)
    Q_PROPERTY(double  brightness  READ brightness  WRITE setBrightness  NOTIFY optionsChanged)
    Q_PROPERTY(int     tier        READ tier        NOTIFY stateChanged)
    Q_PROPERTY(QString badge       READ badge       NOTIFY stateChanged)
    Q_PROPERTY(QString scaleLabel  READ scaleLabel  NOTIFY stateChanged)
    Q_PROPERTY(QString mismatch    READ mismatch    NOTIFY stateChanged)
    Q_PROPERTY(bool    dual        READ dual        NOTIFY stateChanged)
    // [{label, x, y, full}] — colour-bar targets (75 % and 100 %).
    Q_PROPERTY(QVariantList targets  READ targets  NOTIFY stateChanged)
    // [{name, points: [x0, y0, x1, y1, …]}] — gamut hexagons (Input tier).
    Q_PROPERTY(QVariantList hexagons READ hexagons NOTIFY stateChanged)
    // Skin-tone line end point (from the centre), or empty when hidden.
    Q_PROPERTY(QVariantList skinLine READ skinLine NOTIFY stateChanged)
    // "image://qcvscope/<serial>" of the newest trace, "" before the first.
    Q_PROPERTY(QString imageSource READ imageSource NOTIFY imageChanged)

    // Waveform (luma, column × level): same tier / scale / options as the
    // vectorscope, its own panel, active flag and image.
    Q_PROPERTY(bool    waveformActive READ waveformActive WRITE setWaveformActive NOTIFY activeChanged)
    Q_PROPERTY(QString waveformImageSource READ waveformImageSource NOTIFY imageChanged)
    // [{y, label, major}] — level lines in 0..1 of the face (top = 0):
    // 0–100 % for SDR / Signal, nits for HDR.
    Q_PROPERTY(QVariantList waveformLines READ waveformLines NOTIFY stateChanged)
    // What the waveform plots: "Signal Y′ · BT.709 matrix" for SDR (the
    // file's own levels) or "<tier badge> · PQ" for HDR.
    Q_PROPERTY(QString waveformBadge READ waveformBadge NOTIFY stateChanged)
    // True when the waveform is on the PQ nits scale (HDR / linear / log).
    Q_PROPERTY(bool    waveformHdr READ waveformHdr NOTIFY stateChanged)
    // PQ scale's top of face in nits: 300, 600, 1000, 2000 or 4000 (persisted).
    Q_PROPERTY(int     waveformPeak READ waveformPeak WRITE setWaveformPeak NOTIFY optionsChanged)
    // Manual scale: 0 = Auto (from the interpretation), 1 = force the SDR
    // percent scale, 2 = force the nits scale. Shared by both scopes;
    // persisted. A forced nits scale needs a convertible tier — on Signal
    // (nothing resolvable) the scope stays on Y′ and says so.
    Q_PROPERTY(int     scaleMode READ scaleMode WRITE setScaleMode NOTIFY optionsChanged)
    // [{side, frame, clip, tooltip}] — the waveform's peak level per side
    // (side "" single, "A" / "B" dual), measured over every source pixel:
    // this frame, and the highest since the clip / interpretation changed
    // or resetClipPeaks ("Max" in the panel; `clip` = that value). Nits on the HDR scale, % Y′ on SDR; the
    // brightest channel in the tooltip.
    Q_PROPERTY(QVariantList waveformPeaks READ waveformPeaks NOTIFY peaksChanged)

public:
    using RendererFn = std::function<IPlayerRenderer *()>;

    ScopeController(OCIOConfigManager *ocio, ProjectManager *project,
                    RendererFn renderer, QObject *parent = nullptr);

    // The media item A shows: in playlist mode the current clip's source,
    // not the playlist (whose item carries no video tags). Unset = the
    // project's active item.
    void setMediaItemIdFn(std::function<QString()> fn) { m_mediaItemIdFn = std::move(fn); }

    void setImageProvider(BackdropImageProvider *p) { m_provider = p; }
    void setWaveformImageProvider(BackdropImageProvider *p) { m_waveProvider = p; }
    // Dual view: overlay A (cyan) and B (orange).
    void setDualView(bool dual);

    bool    active() const      { return m_active; }
    void    setActive(bool on);
    int     zoom() const        { return m_zoom; }
    void    setZoom(int z);
    bool    colorize() const    { return m_colorize; }
    void    setColorize(bool on);
    double  brightness() const  { return m_brightness; }
    void    setBrightness(double b);

    int     tier() const        { return static_cast<int>(m_config.tier); }
    QString badge() const       { return m_badge; }
    QString scaleLabel() const  { return m_scaleLabel; }
    QString mismatch() const    { return m_mismatch; }
    bool    dual() const        { return m_config.dual; }
    QVariantList targets() const  { return m_targets; }
    QVariantList hexagons() const { return m_hexagons; }
    QVariantList skinLine() const { return m_skinLine; }
    QString imageSource() const { return m_imageSource; }
    bool    waveformActive() const { return m_waveActive; }
    void    setWaveformActive(bool on);
    QString waveformImageSource() const { return m_waveImageSource; }
    QVariantList waveformLines() const { return m_waveLines; }
    QString waveformBadge() const { return m_waveBadge; }
    bool    waveformHdr() const { return m_waveHdr; }
    int     scaleMode() const   { return m_scaleMode; }
    void    setScaleMode(int mode);
    int     waveformPeak() const { return m_wavePeak; }
    void    setWaveformPeak(int nits);
    QVariantList waveformPeaks() const { return m_wavePeaks; }
    Q_INVOKABLE void resetClipPeaks();

public slots:
    // Re-resolve tier / colourspace / geometry and push to the renderer.
    void refresh();

signals:
    void activeChanged();
    void optionsChanged();
    void stateChanged();
    void imageChanged();
    void peaksChanged();

private:
    void resolve();
    void buildGeometry();
    void push();
    void poll();
    void updatePeaks(const ScopePeaks &p);
    QString mediaItemIdA() const;

    OCIOConfigManager     *m_ocio = nullptr;
    ProjectManager        *m_project = nullptr;
    RendererFn             m_renderer;
    std::function<QString()> m_mediaItemIdFn;
    BackdropImageProvider *m_provider = nullptr;
    BackdropImageProvider *m_waveProvider = nullptr;
    QTimer                 m_pollTimer;

    bool   m_active = false;
    int    m_zoom = 1;
    bool   m_colorize = false;
    double m_brightness = 1.0;
    bool   m_dualView = false;
    bool   m_waveActive = false;

    ScopeConfig  m_config;
    bool         m_hdrScale = false;   // either side needs the HDR scale
    int          m_scaleMode = 0;      // 0 auto, 1 force SDR %, 2 force nits
    QString      m_badge;
    QString      m_scaleLabel;
    QString      m_mismatch;
    QVariantList m_targets;
    QVariantList m_hexagons;
    QVariantList m_skinLine;
    QString      m_imageSource;
    quint64      m_lastSerial = 0;
    ScopeConfig  m_waveConfig;
    QVariantList m_waveLines;
    QString      m_waveBadge;
    bool         m_waveHdr = false;
    int          m_wavePeak = 1000;
    QVariantList m_wavePeaks;
    QString      m_peakKey;
    QString      m_gateItemA;       // the active item when the frame gate was set
    quint32      m_peakEpoch = 0;      // bumped per reset (ScopeConfig::peakEpoch)
    quint64      m_peakMinStamp = 0;   // clip change: ignore frames up to this count
    int          m_clipSides = 0;
    bool         m_clipHdr = false;
    float        m_clipLevel[2]   = {0.0f, 0.0f};
    float        m_clipChannel[2] = {0.0f, 0.0f};
    QString      m_waveImageSource;
    quint64      m_lastWaveSerial = 0;
};

} // namespace qcv
