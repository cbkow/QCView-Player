#include "mincolor_engine.h"
#include "linear_stage.h"

#include <QSettings>
#include <QtLogging>

#include <algorithm>
#include <iterator>

namespace qcv {

namespace {
const QString kSettingsKey = QStringLiteral("minColor/chain");
}

MinColorEngine::MinColorEngine(QObject *parent)
    : QObject(parent)
{
    const QVariantMap saved = QSettings().value(kSettingsKey).toMap();
    if (!saved.isEmpty()) m_chain = MinColorChain::fromVariant(saved);
    // One QSettings write per burst of edits, not one per slider tick
    // (the plist flush is synchronous).
    m_persistTimer.setSingleShot(true);
    m_persistTimer.setInterval(400);
    connect(&m_persistTimer, &QTimer::timeout, this,
            [this] { QSettings().setValue(kSettingsKey, m_chain.toVariant()); });
}

void MinColorEngine::emitGroup(GroupSignal group)
{
    if (group) emit (this->*group)();
    emit chainChanged();
}

template <typename T>
void MinColorEngine::set(T &field, const T &value, GroupSignal group)
{
    if (field == value) return;
    field = value;
    persist();
    emitGroup(group);
}

void MinColorEngine::persist()
{
    m_persistTimer.start();
}

void MinColorEngine::setChain(const MinColorChain &c)
{
    if (m_chain == c) return;
    m_chain = c;
    persist();
    emit inputChanged();
    emit kneeChanged();
    emit agxChanged();
    emit outputChanged();
    emit chainChanged();
}

// ---- Clip side: pins ----------------------------------------------------

MinColorChain MinColorEngine::resolveFor(const QString &clipId) const
{
    MinColorChain c = m_chain;
    if (clipId.isEmpty()) return c;
    const auto it = m_pins.constFind(clipId);
    if (it == m_pins.constEnd()) return c;
    if (it->input) c.input = *it->input;
    if (it->knee)  c.knee  = *it->knee;
    return c;
}

bool MinColorEngine::inputPinned() const
{
    const auto it = m_pins.constFind(focusClipId());
    return it != m_pins.constEnd() && it->input.has_value();
}

bool MinColorEngine::kneePinned() const
{
    const auto it = m_pins.constFind(focusClipId());
    return it != m_pins.constEnd() && it->knee.has_value();
}

void MinColorEngine::notePinEdit(const QString &clipId, GroupSignal group)
{
    ++m_pinsRevision;
    emit pinsRevisionChanged();
    emit pinsChanged(clipId);
    emitGroup(group);
}

template <typename T>
void MinColorEngine::setInputField(T MinColorInput::*field, const T &value)
{
    const QString clip = focusClipId();
    if (clip.isEmpty()) { set(m_chain.input.*field, value, &MinColorEngine::inputChanged); return; }
    MinColorPin &pin = m_pins[clip];
    if (!pin.input) pin.input = resolveFor(clip).input;
    if ((*pin.input).*field == value) return;
    (*pin.input).*field = value;
    notePinEdit(clip, &MinColorEngine::inputChanged);
}

template <typename T>
void MinColorEngine::setKneeField(T MinColorKnee::*field, const T &value)
{
    const QString clip = focusClipId();
    if (clip.isEmpty()) { set(m_chain.knee.*field, value, &MinColorEngine::kneeChanged); return; }
    MinColorPin &pin = m_pins[clip];
    if (!pin.knee) pin.knee = resolveFor(clip).knee;
    if ((*pin.knee).*field == value) return;
    (*pin.knee).*field = value;
    notePinEdit(clip, &MinColorEngine::kneeChanged);
}

void MinColorEngine::setInputGamut(int v)    { setInputField(&MinColorInput::gamut, std::clamp(v, 0, DRT_IN_GAMUT_COUNT - 1)); }
void MinColorEngine::setInputTransfer(int v) { setInputField(&MinColorInput::transfer, std::clamp(v, 0, DRT_OETF_INVERSE_FIRST - 1)); }
void MinColorEngine::setInputLimited(bool v) { setInputField(&MinColorInput::limited, v); }

void MinColorEngine::setKneeEnabled(bool v)      { setKneeField(&MinColorKnee::enabled, v); }
void MinColorEngine::setKneeSourceNits(double v) { setKneeField(&MinColorKnee::sourceNits, float(std::clamp(v, 1.0, 10000.0))); }
void MinColorEngine::setKneeTargetNits(double v) { setKneeField(&MinColorKnee::targetNits, float(std::clamp(v, 1.0, 10000.0))); }
void MinColorEngine::setKneeStart(double v)      { setKneeField(&MinColorKnee::start, float(v < 0.0 ? -1.0 : std::clamp(v, 0.0, 0.99))); }

void MinColorEngine::setSlotPinned(const QString &slot, bool pinned)
{
    const QString clip = focusClipId();
    if (clip.isEmpty()) return;
    const MinColorChain cur = resolveFor(clip);
    MinColorPin &pin = m_pins[clip];
    if (slot == QLatin1String("mcInput")) {
        pin.input = pinned ? std::optional<MinColorInput>(cur.input) : std::nullopt;
    } else if (slot == QLatin1String("mcKnee")) {
        pin.knee = pinned ? std::optional<MinColorKnee>(cur.knee) : std::nullopt;
    } else {
        qWarning("MinColorEngine: unknown slot '%s'", qPrintable(slot));
        return;
    }
    if (pin.empty()) m_pins.remove(clip);
    notePinEdit(clip, slot == QLatin1String("mcInput") ? &MinColorEngine::inputChanged
                                                       : &MinColorEngine::kneeChanged);
}

QVariantMap MinColorEngine::clipPinsVariant(const QString &clipId) const
{
    return m_pins.value(clipId).toVariant();
}

void MinColorEngine::replaceAllPins(const QHash<QString, QVariantMap> &pins)
{
    m_pins.clear();
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) {
        const MinColorPin p = MinColorPin::fromVariant(it.value());
        if (!p.empty()) m_pins.insert(it.key(), p);
    }
    ++m_pinsRevision;
    emit pinsRevisionChanged();
    emit inputChanged();
    emit kneeChanged();
    emit chainChanged();
}

QString MinColorEngine::clipBadge(const QString &clipId) const
{
    const auto it = m_pins.constFind(clipId);
    if (clipId.isEmpty() || it == m_pins.constEnd() || it->empty()) return {};
    QStringList parts;
    if (it->input) {
        // The gamut, and the transfer when it isn't the gamut's own
        // display curve or linear — "Rec.709", "ARRI WG4 LogC4".
        QString g = mincolor::inputGamutNames().value(it->input->gamut);
        g.replace(QStringLiteral("Wide Gamut"), QStringLiteral("WG"));
        const int t = it->input->transfer;
        const bool plain = t == DRT_OETF_LINEAR || t == DRT_OETF_REC1886 || t == DRT_OETF_SRGB
                        || t == DRT_OETF_POWER_2_2;
        // Short transfer names, by DRT_OETF_* index (opendrt_params.h).
        static const char *const kShort[] = {
            "Linear", "DaVinci", "T-Log", "ACEScct", "LogC3", "LogC4", "Log3G10", "V-Log",
            "S-Log3", "F-Log2", "1886", "sRGB", "2.2", "709 cam", "PQ", "HLG"};
        const QString tn = (t >= 0 && t < int(std::size(kShort))) ? QString::fromLatin1(kShort[t])
                                                                  : QString::number(t);
        parts << (plain ? g : g + QLatin1Char(' ') + tn);
    }
    if (it->knee && it->knee->enabled) parts << tr("Knee");
    return parts.isEmpty() ? tr("Clip chain") : parts.join(QStringLiteral(" + "));
}

QString MinColorEngine::clipBadgeTooltip(const QString &clipId) const
{
    const auto it = m_pins.constFind(clipId);
    if (clipId.isEmpty() || it == m_pins.constEnd() || it->empty()) return {};
    QStringList lines{tr("Set on this clip (minColor):")};
    if (it->input) {
        lines << tr("Input: %1 · %2%3")
                     .arg(mincolor::inputGamutNames().value(it->input->gamut),
                          mincolor::inputTransferNames().value(it->input->transfer),
                          it->input->limited ? tr(" · limited range") : QString());
    }
    if (it->knee) {
        lines << (it->knee->enabled
                      ? tr("Highlight Knee: on · %1 → %2 nits")
                            .arg(qRound(it->knee->sourceNits)).arg(qRound(it->knee->targetNits))
                      : tr("Highlight Knee: off"));
    }
    return lines.join(QLatin1Char('\n'));
}

void MinColorEngine::setEdrLinear(bool on)
{
    if (m_edrLinear == on) return;
    m_edrLinear = on;
    emit outputChanged();
    emit chainChanged();
}

void MinColorEngine::setAgxEnabled(bool v)      { set(m_chain.agx.enabled, v, &MinColorEngine::agxChanged); }
void MinColorEngine::setAgxTarget(int v)        { set(m_chain.agx.target, std::clamp(v, 0, 2), &MinColorEngine::agxChanged); }
void MinColorEngine::setAgxPeak(double v)       { set(m_chain.agx.peak, float(std::clamp(v, 100.0, 10000.0)), &MinColorEngine::agxChanged); }
void MinColorEngine::setAgxWhiteEv(double v)    { set(m_chain.agx.whiteEv, float(std::clamp(v, 1.0, 20.0)), &MinColorEngine::agxChanged); }
void MinColorEngine::setAgxBlackEv(double v)    { set(m_chain.agx.blackEv, float(std::clamp(v, -20.0, -1.0)), &MinColorEngine::agxChanged); }
void MinColorEngine::setAgxContrast(double v)   { set(m_chain.agx.contrast, float(std::clamp(v, 0.5, 5.0)), &MinColorEngine::agxChanged); }
void MinColorEngine::setAgxToe(double v)        { set(m_chain.agx.toe, float(std::clamp(v, 0.5, 5.0)), &MinColorEngine::agxChanged); }
void MinColorEngine::setAgxShoulder(double v)   { set(m_chain.agx.shoulder, float(std::clamp(v, 0.5, 5.0)), &MinColorEngine::agxChanged); }
void MinColorEngine::setAgxHueRestore(double v) { set(m_chain.agx.hueRestore, float(std::clamp(v, 0.0, 1.0)), &MinColorEngine::agxChanged); }
void MinColorEngine::setAgxHdrPurity(double v)  { set(m_chain.agx.hdrPurity, float(std::clamp(v, 0.0, 1.0)), &MinColorEngine::agxChanged); }

void MinColorEngine::setOpenDrt(bool v)        { set(m_chain.output.openDrt, v, &MinColorEngine::outputChanged); }
void MinColorEngine::setLook(int v)            { set(m_chain.output.look, std::clamp(v, 0, drt::kLookCount - 1), &MinColorEngine::outputChanged); }
void MinColorEngine::setTonescale(int v)       { set(m_chain.output.tonescale, std::clamp(v, 0, drt::kTonescaleCount), &MinColorEngine::outputChanged); }
void MinColorEngine::setCreativeWhite(int v)   { set(m_chain.output.cwp, std::clamp(v, 0, drt::kCwpCount), &MinColorEngine::outputChanged); }
void MinColorEngine::setCreativeWhiteLimit(double v) { set(m_chain.output.cwpLimit, float(std::clamp(v, 0.0, 1.0)), &MinColorEngine::outputChanged); }
void MinColorEngine::setDisplay(int v)         { set(m_chain.output.display, std::clamp(v, 0, drt::kDisplayCount - 1), &MinColorEngine::outputChanged); }
void MinColorEngine::setSurround(int v)        { set(m_chain.output.surround, std::clamp(v, 0, 2), &MinColorEngine::outputChanged); }
void MinColorEngine::setPeakNits(double v)     { set(m_chain.output.peakNits, float(std::clamp(v, 48.0, 10000.0)), &MinColorEngine::outputChanged); }
void MinColorEngine::setGreyBoost(double v)    { set(m_chain.output.greyBoost, float(std::clamp(v, 0.0, 1.0)), &MinColorEngine::outputChanged); }
void MinColorEngine::setHdrPurity(double v)    { set(m_chain.output.hdrPurity, float(std::clamp(v, 0.0, 1.0)), &MinColorEngine::outputChanged); }
void MinColorEngine::setGreyNits(double v)     { set(m_chain.output.greyNits, float(std::clamp(v, 1.0, 100.0)), &MinColorEngine::outputChanged); }

int MinColorEngine::displayKindOf(int index) const
{
    if (index < 0 || index >= drt::kDisplayCount) return 0;
    switch (drt::kDisplays[index].eotf) {
    case DRT_EOTF_PQ:     return 1;
    case DRT_EOTF_HLG:    return 2;
    case DRT_EOTF_LINEAR: return 3;
    default:              return 0;
    }
}

int MinColorEngine::displayKind() const
{
    return displayKindOf(m_chain.output.display);
}

// The knee's effective start (BT.2390's for the current peaks when the
// user hasn't set one) and its nits read-out — linear_stage's maths, the
// same the OCIO stage and the vendored drt_knee use.
double MinColorEngine::kneeStartEffective() const
{
    const MinColorKnee k = focused().knee;   // the focused clip's pin, else the default
    if (k.start >= 0.0f) return k.start;
    const float src = std::max(k.sourceNits, 1.0f);
    const float tgt = displayIsSdr() ? 100.0f : std::max(k.targetNits, 1.0f);
    const float maxLum = linear_stage::pqEncode(tgt / 10000.0f) / linear_stage::pqEncode(src / 10000.0f);
    return linear_stage::bt2390KneeStart(maxLum) / std::max(maxLum, 1e-6f);
}

double MinColorEngine::kneeStartNits() const
{
    const MinColorKnee k = focused().knee;
    const float src = std::max(k.sourceNits, 1.0f);
    const float tgt = displayIsSdr() ? 100.0f : std::max(k.targetNits, 1.0f);
    const float maxLum = linear_stage::pqEncode(tgt / 10000.0f) / linear_stage::pqEncode(src / 10000.0f);
    const float ks = float(kneeStartEffective()) * maxLum;
    return linear_stage::kneeStartNits(ks, src);
}

} // namespace qcv
