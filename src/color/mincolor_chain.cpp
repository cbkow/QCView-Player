#include "mincolor_chain.h"

#include <algorithm>
#include <cmath>

namespace qcv {

namespace {

int agxTargetGamut(int target)
{
    switch (target) {
    case 0:  return DRT_IN_REC2020;
    case 2:  return DRT_IN_P3D65;
    default: return DRT_IN_REC709;
    }
}

int clampIndex(int v, int count) { return (v >= 0 && v < count) ? v : 0; }

} // namespace

QVariantMap MinColorChain::toVariant() const
{
    QVariantMap m;
    QVariantMap in;
    in[QStringLiteral("gamut")]    = input.gamut;
    in[QStringLiteral("transfer")] = input.transfer;
    in[QStringLiteral("limited")]  = input.limited;
    m[QStringLiteral("input")] = in;
    QVariantMap kn;
    kn[QStringLiteral("enabled")]    = knee.enabled;
    kn[QStringLiteral("sourceNits")] = double(knee.sourceNits);
    kn[QStringLiteral("targetNits")] = double(knee.targetNits);
    kn[QStringLiteral("start")]      = double(knee.start);
    m[QStringLiteral("knee")] = kn;
    QVariantMap ax;
    ax[QStringLiteral("enabled")]    = agx.enabled;
    ax[QStringLiteral("target")]     = agx.target;
    ax[QStringLiteral("peak")]       = double(agx.peak);
    ax[QStringLiteral("whiteEv")]    = double(agx.whiteEv);
    ax[QStringLiteral("blackEv")]    = double(agx.blackEv);
    ax[QStringLiteral("contrast")]   = double(agx.contrast);
    ax[QStringLiteral("toe")]        = double(agx.toe);
    ax[QStringLiteral("shoulder")]   = double(agx.shoulder);
    ax[QStringLiteral("hueRestore")] = double(agx.hueRestore);
    ax[QStringLiteral("hdrPurity")]  = double(agx.hdrPurity);
    ax[QStringLiteral("outset")]     = double(agx.outset);
    m[QStringLiteral("agx")] = ax;
    QVariantMap out;
    out[QStringLiteral("openDrt")]   = output.openDrt;
    out[QStringLiteral("look")]      = output.look;
    out[QStringLiteral("tonescale")] = output.tonescale;
    out[QStringLiteral("cwp")]       = output.cwp;
    out[QStringLiteral("cwpLimit")]  = double(output.cwpLimit);
    out[QStringLiteral("display")]   = output.display;
    out[QStringLiteral("surround")]  = output.surround;
    out[QStringLiteral("peakNits")]  = double(output.peakNits);
    out[QStringLiteral("greyBoost")] = double(output.greyBoost);
    out[QStringLiteral("hdrPurity")] = double(output.hdrPurity);
    out[QStringLiteral("greyNits")]  = double(output.greyNits);
    m[QStringLiteral("output")] = out;
    return m;
}

MinColorChain MinColorChain::fromVariant(const QVariantMap &m)
{
    MinColorChain c;
    const QVariantMap in = m.value(QStringLiteral("input")).toMap();
    if (!in.isEmpty()) {
        c.input.gamut    = clampIndex(in.value(QStringLiteral("gamut"), c.input.gamut).toInt(), DRT_IN_GAMUT_COUNT);
        c.input.transfer = clampIndex(in.value(QStringLiteral("transfer"), c.input.transfer).toInt(), DRT_OETF_INVERSE_FIRST);
        c.input.limited  = in.value(QStringLiteral("limited"), false).toBool();
    }
    const QVariantMap kn = m.value(QStringLiteral("knee")).toMap();
    if (!kn.isEmpty()) {
        c.knee.enabled    = kn.value(QStringLiteral("enabled"), false).toBool();
        c.knee.sourceNits = float(kn.value(QStringLiteral("sourceNits"), 1000.0).toDouble());
        c.knee.targetNits = float(kn.value(QStringLiteral("targetNits"), 100.0).toDouble());
        c.knee.start      = float(kn.value(QStringLiteral("start"), -1.0).toDouble());
    }
    const QVariantMap ax = m.value(QStringLiteral("agx")).toMap();
    if (!ax.isEmpty()) {
        c.agx.enabled    = ax.value(QStringLiteral("enabled"), false).toBool();
        c.agx.target     = clampIndex(ax.value(QStringLiteral("target"), 1).toInt(), 3);
        c.agx.peak       = float(ax.value(QStringLiteral("peak"), 100.0).toDouble());
        c.agx.whiteEv    = float(ax.value(QStringLiteral("whiteEv"), 6.5).toDouble());
        c.agx.blackEv    = float(ax.value(QStringLiteral("blackEv"), -10.0).toDouble());
        c.agx.contrast   = float(ax.value(QStringLiteral("contrast"), 2.4).toDouble());
        c.agx.toe        = float(ax.value(QStringLiteral("toe"), 1.5).toDouble());
        c.agx.shoulder   = float(ax.value(QStringLiteral("shoulder"), 1.5).toDouble());
        c.agx.hueRestore = float(ax.value(QStringLiteral("hueRestore"), 0.6).toDouble());
        c.agx.hdrPurity  = float(ax.value(QStringLiteral("hdrPurity"), 0.5).toDouble());
        c.agx.outset     = float(ax.value(QStringLiteral("outset"), 1.0).toDouble());
    }
    const QVariantMap out = m.value(QStringLiteral("output")).toMap();
    if (!out.isEmpty()) {
        c.output.openDrt   = out.value(QStringLiteral("openDrt"), false).toBool();
        c.output.look      = clampIndex(out.value(QStringLiteral("look"), 0).toInt(), drt::kLookCount);
        c.output.tonescale = std::clamp(out.value(QStringLiteral("tonescale"), 0).toInt(), 0, drt::kTonescaleCount);
        c.output.cwp       = std::clamp(out.value(QStringLiteral("cwp"), 0).toInt(), 0, drt::kCwpCount);
        c.output.cwpLimit  = float(out.value(QStringLiteral("cwpLimit"), 0.25).toDouble());
        c.output.display   = clampIndex(out.value(QStringLiteral("display"), 1).toInt(), drt::kDisplayCount);
        c.output.surround  = std::clamp(out.value(QStringLiteral("surround"), 0).toInt(), 0, 2);
        c.output.peakNits  = float(out.value(QStringLiteral("peakNits"), 100.0).toDouble());
        c.output.greyBoost = float(out.value(QStringLiteral("greyBoost"), 0.13).toDouble());
        c.output.hdrPurity = float(out.value(QStringLiteral("hdrPurity"), 0.5).toDouble());
        c.output.greyNits  = float(out.value(QStringLiteral("greyNits"), 10.0).toDouble());
    }
    return c;
}

namespace mincolor {

MinColorGpu resolve(const MinColorChain &chain, bool sdrCapture, bool edrLinear)
{
    MinColorGpu g;

    // ---- Input half: the file's encoding into linear Rec.2020, plus the
    // knee's fields (drt_knee reads them off the same block).
    drt::DrtParams in = drt::drt_stickshift_defaults();
    in.in_gamut      = clampIndex(chain.input.gamut, DRT_IN_GAMUT_COUNT);
    in.in_oetf       = clampIndex(chain.input.transfer, DRT_OETF_INVERSE_FIRST);
    in.in_range      = chain.input.limited ? 1 : 0;
    in.working_gamut = DRT_IN_REC2020;
    in.kn_src        = std::max(chain.knee.sourceNits, 1.0f);
    in.kn_tgt        = std::max(chain.knee.targetNits, 1.0f);
    in.kn_auto       = chain.knee.start < 0.0f ? 1 : 0;
    in.kn_start      = std::clamp(chain.knee.start, 0.0f, 0.99f);
    in = drt::drt_derive(in);
    in = drt::drt_knee_derive(in);
    g.in = in;

    // ---- AgX (view). Works from the working gamut; lands in its target
    // gamut as display-linear light, 1.0 = 100 nits.
    drt::DrtAgxParams agx = drt::drt_agx_defaults();
    agx.working_gamut  = float(DRT_IN_REC2020);
    agx.target         = float(clampIndex(chain.agx.target, 3));
    agx.peak           = sdrCapture ? 100.0f : std::max(chain.agx.peak, 100.0f);
    agx.white_ev       = chain.agx.whiteEv;
    agx.black_ev       = chain.agx.blackEv;
    agx.contrast       = chain.agx.contrast;
    agx.toe_power      = chain.agx.toe;
    agx.shoulder_power = chain.agx.shoulder;
    agx.hue_restore    = chain.agx.hueRestore;
    agx.hdr_purity     = chain.agx.hdrPurity;
    agx.outset         = chain.agx.outset;
    g.agx = drt::drt_agx_derive(agx);

    // ---- Output half: the rendering and the display encoding, exactly
    // the DCTL's preset-mode block (drt_resolve), with the chain's
    // surround and view on top. With AgX on the picture formation has
    // happened: un-tone-mapped from AgX's target gamut, encode only.
    const bool agxOn   = chain.agx.enabled;
    const bool openDrt = chain.output.openDrt && !agxOn;
    drt::DrtSettings s;
    s.in_gamut  = agxOn ? agxTargetGamut(chain.agx.target) : DRT_IN_REC2020;
    s.in_oetf   = DRT_OETF_LINEAR;
    s.tn_Lp     = sdrCapture ? 100.0f : std::clamp(chain.output.peakNits, 48.0f, 10000.0f);
    s.tn_gb     = chain.output.greyBoost;
    s.pt_hdr    = chain.output.hdrPurity;
    s.tn_Lg     = chain.output.greyNits;
    s.look      = clampIndex(chain.output.look, drt::kLookCount);
    s.tonescale = std::clamp(chain.output.tonescale, 0, drt::kTonescaleCount);
    s.cwp       = std::clamp(chain.output.cwp, 0, drt::kCwpCount);
    s.cwp_lm    = chain.output.cwpLimit;
    // The sRGB 2.2 / Rec.709 preset for captures (kDisplays[1]).
    s.display   = sdrCapture ? 1 : clampIndex(chain.output.display, drt::kDisplayCount);
    drt::DrtParams out = drt::drt_resolve(s);
    // Surround is the viewing room, set by hand (presets never write it),
    // and the view is ours: both are read per pixel, not derived, so they
    // can follow drt_resolve. Surround does feed the tonescale constants,
    // so derive again when it differs from the preset's.
    out.out_view = openDrt ? 0 : 1;
    if (out.tn_su != chain.output.surround) {
        out.tn_su = std::clamp(chain.output.surround, 0, 2);
        out = drt::drt_derive(out);
    }
    g.out = out;

    g.flags.kneeOn = chain.knee.enabled ? 1 : 0;
    g.flags.agxOn  = agxOn ? 1 : 0;
    // EDR (display-linear swapchain, 1.0 = 100 nits): OpenDRT's linear
    // hand-off puts its peak at 1.0 (ts_dsc = 100 / Lp), so scale by
    // peak / 100. Un-tone-mapped and AgX already put 100 nits at 1.0.
    const bool linearOut = drt::kDisplays[s.display].eotf == DRT_EOTF_LINEAR;
    g.flags.outScale = (edrLinear && linearOut && openDrt) ? s.tn_Lp / 100.0f : 1.0f;
    return g;
}

void apply(const MinColorGpu &g, float *rgb)
{
    drt::float3 c = drt::make_float3(rgb[0], rgb[1], rgb[2]);
    c = drt::drt_input_transform(g.in, c);
    if (g.flags.kneeOn) c = drt::drt_knee(g.in, c);
    if (g.flags.agxOn)  c = drt::drt_agx(g.agx, c);
    c = drt::drt_transform(g.out, c);
    rgb[0] = c.x * g.flags.outScale;
    rgb[1] = c.y * g.flags.outScale;
    rgb[2] = c.z * g.flags.outScale;
}

int linearDisplayIndex(int displayGamut)
{
    for (int i = 0; i < drt::kDisplayCount; ++i) {
        if (drt::kDisplays[i].eotf == DRT_EOTF_LINEAR && drt::kDisplays[i].display_gamut == displayGamut)
            return i;
    }
    return -1;
}

QStringList inputGamutNames()
{
    QStringList out;
    for (int i = 0; i < DRT_IN_GAMUT_COUNT; ++i) out << QString::fromUtf8(drt::kInGamutNames[i]);
    return out;
}

QStringList inputTransferNames()
{
    QStringList out;
    for (int i = 0; i < DRT_OETF_INVERSE_FIRST; ++i) out << QString::fromUtf8(drt::kInOetfNames[i]);
    return out;
}

QStringList lookNames()
{
    QStringList out;
    for (int i = 0; i < drt::kLookCount; ++i) out << QString::fromUtf8(drt::kLooks[i].name);
    return out;
}

QStringList tonescaleNames()
{
    QStringList out;
    out << QStringLiteral("Use look");
    for (int i = 0; i < drt::kTonescaleCount; ++i) out << QString::fromUtf8(drt::kTonescales[i].name);
    return out;
}

QStringList creativeWhiteNames()
{
    QStringList out;
    out << QStringLiteral("Use look");
    for (int i = 0; i < drt::kCwpCount; ++i) out << QString::fromUtf8(drt::kCwpNames[i]);
    return out;
}

QStringList displayNames()
{
    QStringList out;
    for (int i = 0; i < drt::kDisplayCount; ++i) out << QString::fromUtf8(drt::kDisplays[i].name);
    return out;
}

} // namespace mincolor

} // namespace qcv
