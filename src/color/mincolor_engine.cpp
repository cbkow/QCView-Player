#include "mincolor_engine.h"
#include "linear_stage.h"

#include <QSettings>

#include <algorithm>

namespace qcv {

namespace {
const QString kSettingsKey = QStringLiteral("minColor/chain");
}

MinColorEngine::MinColorEngine(QObject *parent)
    : QObject(parent)
{
    const QVariantMap saved = QSettings().value(kSettingsKey).toMap();
    if (!saved.isEmpty()) m_chain = MinColorChain::fromVariant(saved);
}

template <typename T>
void MinColorEngine::set(T &field, const T &value)
{
    if (field == value) return;
    field = value;
    persist();
    emit chainChanged();
}

void MinColorEngine::persist() const
{
    QSettings().setValue(kSettingsKey, m_chain.toVariant());
}

void MinColorEngine::setChain(const MinColorChain &c)
{
    if (m_chain == c) return;
    m_chain = c;
    persist();
    emit chainChanged();
}

void MinColorEngine::setInputGamut(int v)    { set(m_chain.input.gamut, std::clamp(v, 0, DRT_IN_GAMUT_COUNT - 1)); }
void MinColorEngine::setInputTransfer(int v) { set(m_chain.input.transfer, std::clamp(v, 0, DRT_OETF_INVERSE_FIRST - 1)); }
void MinColorEngine::setInputLimited(bool v) { set(m_chain.input.limited, v); }

void MinColorEngine::setKneeEnabled(bool v)      { set(m_chain.knee.enabled, v); }
void MinColorEngine::setKneeSourceNits(double v) { set(m_chain.knee.sourceNits, float(std::clamp(v, 1.0, 10000.0))); }
void MinColorEngine::setKneeTargetNits(double v) { set(m_chain.knee.targetNits, float(std::clamp(v, 1.0, 10000.0))); }
void MinColorEngine::setKneeStart(double v)      { set(m_chain.knee.start, float(v < 0.0 ? -1.0 : std::clamp(v, 0.0, 0.99))); }

void MinColorEngine::setAgxEnabled(bool v)      { set(m_chain.agx.enabled, v); }
void MinColorEngine::setAgxTarget(int v)        { set(m_chain.agx.target, std::clamp(v, 0, 2)); }
void MinColorEngine::setAgxPeak(double v)       { set(m_chain.agx.peak, float(std::clamp(v, 100.0, 10000.0))); }
void MinColorEngine::setAgxWhiteEv(double v)    { set(m_chain.agx.whiteEv, float(std::clamp(v, 1.0, 20.0))); }
void MinColorEngine::setAgxBlackEv(double v)    { set(m_chain.agx.blackEv, float(std::clamp(v, -20.0, -1.0))); }
void MinColorEngine::setAgxContrast(double v)   { set(m_chain.agx.contrast, float(std::clamp(v, 0.5, 5.0))); }
void MinColorEngine::setAgxToe(double v)        { set(m_chain.agx.toe, float(std::clamp(v, 0.5, 5.0))); }
void MinColorEngine::setAgxShoulder(double v)   { set(m_chain.agx.shoulder, float(std::clamp(v, 0.5, 5.0))); }
void MinColorEngine::setAgxHueRestore(double v) { set(m_chain.agx.hueRestore, float(std::clamp(v, 0.0, 1.0))); }
void MinColorEngine::setAgxHdrPurity(double v)  { set(m_chain.agx.hdrPurity, float(std::clamp(v, 0.0, 1.0))); }

void MinColorEngine::setOpenDrt(bool v)        { set(m_chain.output.openDrt, v); }
void MinColorEngine::setLook(int v)            { set(m_chain.output.look, std::clamp(v, 0, drt::kLookCount - 1)); }
void MinColorEngine::setTonescale(int v)       { set(m_chain.output.tonescale, std::clamp(v, 0, drt::kTonescaleCount)); }
void MinColorEngine::setCreativeWhite(int v)   { set(m_chain.output.cwp, std::clamp(v, 0, drt::kCwpCount)); }
void MinColorEngine::setCreativeWhiteLimit(double v) { set(m_chain.output.cwpLimit, float(std::clamp(v, 0.0, 1.0))); }
void MinColorEngine::setDisplay(int v)         { set(m_chain.output.display, std::clamp(v, 0, drt::kDisplayCount - 1)); }
void MinColorEngine::setSurround(int v)        { set(m_chain.output.surround, std::clamp(v, 0, 2)); }
void MinColorEngine::setPeakNits(double v)     { set(m_chain.output.peakNits, float(std::clamp(v, 48.0, 10000.0))); }
void MinColorEngine::setGreyBoost(double v)    { set(m_chain.output.greyBoost, float(std::clamp(v, 0.0, 1.0))); }
void MinColorEngine::setHdrPurity(double v)    { set(m_chain.output.hdrPurity, float(std::clamp(v, 0.0, 1.0))); }
void MinColorEngine::setGreyNits(double v)     { set(m_chain.output.greyNits, float(std::clamp(v, 1.0, 100.0))); }

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
    if (m_chain.knee.start >= 0.0f) return m_chain.knee.start;
    const float src = std::max(m_chain.knee.sourceNits, 1.0f);
    const float tgt = displayIsSdr() ? 100.0f : std::max(m_chain.knee.targetNits, 1.0f);
    const float maxLum = linear_stage::pqEncode(tgt / 10000.0f) / linear_stage::pqEncode(src / 10000.0f);
    return linear_stage::bt2390KneeStart(maxLum) / std::max(maxLum, 1e-6f);
}

double MinColorEngine::kneeStartNits() const
{
    const float src = std::max(m_chain.knee.sourceNits, 1.0f);
    const float tgt = displayIsSdr() ? 100.0f : std::max(m_chain.knee.targetNits, 1.0f);
    const float maxLum = linear_stage::pqEncode(tgt / 10000.0f) / linear_stage::pqEncode(src / 10000.0f);
    const float ks = float(kneeStartEffective()) * maxLum;
    return linear_stage::kneeStartNits(ks, src);
}

} // namespace qcv
