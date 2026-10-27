// probe-ocio-emulate — host-display emulation in the chain builder
// (2026-10-27 colour awareness): a stream clip's view is rendered for the
// HOST Blender's display device and view, and the codes are carried to this
// machine's display. Checks, on the bundled Blender 5.2 config:
//   - "true" emulation (display → display) equals the two-processor
//     composition on a grid;
//   - the split build (pre + post) equals the unsplit one;
//   - host display == viewer display is a no-op (no step appended);
//   - "as shown" with an sRGB viewer is the host's view on the host's
//     display, codes untouched (Blender writes those codes into its
//     sRGB-tagged window — measured);
//   - the pin round-trips through toVariant / fromVariant;
//   - sameShader is false when emulation differs;
//   - a host Rec.2100-PQ view emulated onto sRGB clamps to 0..1;
//   - nearestDisplay: P3 primaries + 2.2 → Display P3 with a transfer note;
//     Rec.709 + 2.4 → Rec.1886; BT.2020 → Rec.2020; sRGB → sRGB exact.
//
// Usage:  probe-ocio-emulate <config.ocio>

#include "color/ocio_chain_builder.h"
#include "color/ocio_chain_spec.h"
#include "color/ocio_config_manager.h"

#include <OpenColorIO/OpenColorIO.h>

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

namespace OCIO = OCIO_NAMESPACE;
using namespace qcv;

namespace {

int g_failed = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

OCIO::ConstCPUProcessorRcPtr cpu(OCIO::ConstConfigRcPtr cfg, OCIO::ConstTransformRcPtr t)
{
    return cfg->getProcessor(t)->getDefaultCPUProcessor();
}

// Scene-linear Rec.709 → reference, the clip's Input for these checks.
OcioChainSpec baseSpec(const QString &configPath)
{
    OcioChainSpec s;
    s.configPath  = configPath;
    s.scene.input = QStringLiteral("Linear Rec.709");
    s.display     = QStringLiteral("sRGB");
    s.view        = QStringLiteral("Standard");
    return s;
}

double maxAbsDiff(OCIO::ConstConfigRcPtr cfg, OCIO::ConstTransformRcPtr a, OCIO::ConstTransformRcPtr b)
{
    auto pa = cpu(cfg, a), pb = cpu(cfg, b);
    double m = 0;
    for (int r = 0; r <= 8; ++r) for (int g = 0; g <= 8; ++g) for (int bl = 0; bl <= 8; ++bl) {
        float x[3] = {r / 8.f, g / 8.f, bl / 8.f}, y[3] = {r / 8.f, g / 8.f, bl / 8.f};
        pa->applyRGB(x); pb->applyRGB(y);
        for (int i = 0; i < 3; ++i) m = std::fmax(m, std::fabs(double(x[i]) - double(y[i])));
    }
    return m;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: probe-ocio-emulate <config.ocio>\n");
        return 2;
    }
    const QString path = QString::fromUtf8(argv[1]);
    OCIOConfigManager mgr;
    if (!mgr.loadConfigFile(path)) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    OCIO::ConstConfigRcPtr cfg = OCIO::Config::CreateFromFile(argv[1]);
    const char *interchange = cfg->getRoleColorSpace("aces_interchange");

    // ---- true emulation == composition ----
    OcioChainSpec spec = baseSpec(path);
    spec.scene.hostDisplay = QStringLiteral("Rec.2020");
    spec.scene.hostView    = QStringLiteral("AgX");
    spec.scene.emulate     = true;
    spec.scene.emulateTrue = true;
    QString err;
    OCIO::GroupTransformRcPtr chain = OcioChainBuilder::buildGroupTransform(spec, cfg, &err);
    check(chain != nullptr, "true emulation chain builds");
    {
        OCIO::GroupTransformRcPtr ref = OCIO::GroupTransform::Create();
        OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
        dvt->setSrc("Linear Rec.709"); dvt->setDisplay("Rec.2020"); dvt->setView("AgX");
        ref->appendTransform(dvt);
        OCIO::ColorSpaceTransformRcPtr cst = OCIO::ColorSpaceTransform::Create();
        cst->setSrc("Rec.2020"); cst->setDst("sRGB");
        ref->appendTransform(cst);
        OCIO::RangeTransformRcPtr rng = OCIO::RangeTransform::Create();
        rng->setMinInValue(0); rng->setMinOutValue(0); rng->setMaxInValue(1); rng->setMaxOutValue(1);
        ref->appendTransform(rng);
        check(chain && maxAbsDiff(cfg, chain, ref) < 1e-5, "true emulation == DVT(host) → CST(host display → sRGB) → clamp");
    }
    // ---- split == unsplit ----
    {
        OcioSplitTransforms split;
        const bool ok = OcioChainBuilder::buildSplitTransforms(spec, cfg, split, &err);
        check(ok, "split build with emulation");
        if (ok && chain) {
            OCIO::GroupTransformRcPtr whole = OCIO::GroupTransform::Create();
            whole->appendTransform(split.pre);
            whole->appendTransform(split.post);
            check(maxAbsDiff(cfg, chain, whole) < 1e-4, "split pre+post == unsplit chain");
            check(split.displayIsSdr, "output classified for the VIEWER's display (SDR sRGB), not the host's");
        }
    }
    // ---- "as shown" on an sRGB viewer: the host's codes untouched ----
    {
        OcioChainSpec shown = spec;
        shown.scene.emulateTrue = false;
        OCIO::GroupTransformRcPtr c = OcioChainBuilder::buildGroupTransform(shown, cfg, &err);
        OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
        dvt->setSrc("Linear Rec.709"); dvt->setDisplay("Rec.2020"); dvt->setView("AgX");
        check(c && maxAbsDiff(cfg, c, dvt) < 1e-6, "as-shown on sRGB == host view for the host display, codes untouched");
        // red through the two modes differs (the measured 0.72 vs 0.86 red)
        float a[3] = {1, 0, 0}, b[3] = {1, 0, 0};
        if (c && chain) { cpu(cfg, c)->applyRGB(a); cpu(cfg, chain)->applyRGB(b); }
        check(c && chain && std::fabs(a[0] - b[0]) > 0.05, "as-shown and true emulation differ on a pure red");
    }
    // ---- same display both ends: no step ----
    {
        OcioChainSpec same = spec;
        same.scene.hostDisplay = QStringLiteral("sRGB");
        same.scene.hostView    = QStringLiteral("AgX");
        OcioChainSpec plain = baseSpec(path);
        plain.view = QStringLiteral("AgX");
        OCIO::GroupTransformRcPtr c1 = OcioChainBuilder::buildGroupTransform(same, cfg, &err);
        OCIO::GroupTransformRcPtr c2 = OcioChainBuilder::buildGroupTransform(plain, cfg, &err);
        check(c1 && c2 && maxAbsDiff(cfg, c1, c2) < 1e-6, "host display == viewer display: identical to the plain chain");
    }
    // ---- a PQ host onto sRGB clamps ----
    {
        OcioChainSpec pq = spec;
        pq.scene.hostDisplay = QStringLiteral("Rec.2100-PQ");
        pq.scene.hostView    = QStringLiteral("AgX - HDR 1000 nits");
        pq.scene.emulateTrue = true;
        OCIO::GroupTransformRcPtr c = OcioChainBuilder::buildGroupTransform(pq, cfg, &err);
        bool inRange = c != nullptr;
        if (c) {
            auto p = cpu(cfg, c);
            for (int i = 0; i <= 8 && inRange; ++i) {
                float v[3] = {i * 4.f, i * 4.f, i * 4.f};
                p->applyRGB(v);
                for (float x : v) if (x < -1e-6f || x > 1.0f + 1e-6f) inRange = false;
            }
        }
        check(inRange, "host Rec.2100-PQ view emulated onto sRGB stays within 0..1");
    }
    // ---- an unknown host view: emulation off, chain still builds ----
    {
        OcioChainSpec bad = spec;
        bad.scene.hostView = QStringLiteral("Not A View");
        OcioChainSpec plain = baseSpec(path);
        OCIO::GroupTransformRcPtr c1 = OcioChainBuilder::buildGroupTransform(bad, cfg, &err);
        OCIO::GroupTransformRcPtr c2 = OcioChainBuilder::buildGroupTransform(plain, cfg, &err);
        check(c1 && c2 && maxAbsDiff(cfg, c1, c2) < 1e-6, "unknown host view: builds as the plain chain");
    }
    // ---- pin round-trip, sameShader ----
    {
        OcioScenePin pin;
        pin.hostDisplay = OcioScenePin::HostDisplay{QStringLiteral("Rec.2020"), QStringLiteral("AgX"), true, false};
        const OcioScenePin back = OcioScenePin::fromVariant(pin.toVariant());
        check(back.hostDisplay && back.hostDisplay->display == QStringLiteral("Rec.2020")
                  && back.hostDisplay->view == QStringLiteral("AgX") && back.hostDisplay->on
                  && !back.hostDisplay->trueEmulation && !back.empty(),
              "pin round-trips through toVariant / fromVariant");
        OcioChainSpec a = spec, b = spec;
        b.scene.emulate = false;
        check(!a.sameShader(b), "sameShader false when emulation differs");
        b = spec; b.scene.emulateTrue = false;
        check(!a.sameShader(b), "sameShader false when the mode differs");
    }
    // ---- the manager: set / toggle / clear / badge ----
    {
        const QString clip = QStringLiteral("clip-stream");
        check(!mgr.setHostDisplayForClip(clip, QStringLiteral("Rec.2020"), QStringLiteral("Not A View")),
              "manager refuses a host view the config lacks");
        check(mgr.setHostDisplayForClip(clip, QStringLiteral("Rec.2020"), QStringLiteral("AgX")),
              "manager pins a host display/view the config has");
        OcioChainSpec s = mgr.specForClip(clip);
        check(s.scene.emulate && s.scene.hostDisplay == QStringLiteral("Rec.2020") && s.scene.hostView == QStringLiteral("AgX"),
              "specForClip carries the host display");
        check(mgr.clipBadge(clip).contains(QStringLiteral("Host display")), "badge says Host display");
        mgr.setHostDisplayEnabledForClip(clip, false);
        check(!mgr.specForClip(clip).scene.emulate, "toggle off keeps the pin, stops emulating");
        mgr.setHostDisplayTrueEmulationForClip(clip, true);
        check(mgr.specForClip(clip).scene.emulateTrue, "true-emulation flag reaches the spec");
        check(mgr.clipHostDisplay(clip).value(QStringLiteral("valid")).toBool(), "clipHostDisplay reports valid");
        mgr.setViewContext(clip, false, QString(), QString());
        check(mgr.hostDisplayPinned(), "hostDisplayPinned on the focused clip");
        mgr.setSlotPinned(QStringLiteral("hostDisplay"), false);
        check(!mgr.specForClip(clip).scene.emulate && mgr.clipHostDisplay(clip).isEmpty(), "↺ clears the slot");
    }
    // ---- nearestDisplay ----
    {
        auto xy = [](double rx, double ry, double gx, double gy, double bx, double by, double wx, double wy) {
            return QVariantMap{{QStringLiteral("r"), QVariantList{rx, ry}}, {QStringLiteral("g"), QVariantList{gx, gy}},
                               {QStringLiteral("b"), QVariantList{bx, by}}, {QStringLiteral("w"), QVariantList{wx, wy}}};
        };
        // Display P3's own curve estimates at ~2.22, so a host 2.2 is a match
        // without a note; a 2.6 (DCI-style) host gets the transfer note.
        const QVariantMap p3 = mgr.nearestDisplay(xy(0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290), 2.2);
        check(p3.value(QStringLiteral("display")).toString() == QStringLiteral("Display P3")
                  && p3.value(QStringLiteral("note")).toString().isEmpty(),
              "P3 primaries + 2.2 → Display P3, no note");
        const QVariantMap p3dci = mgr.nearestDisplay(xy(0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290), 2.6);
        check(p3dci.value(QStringLiteral("display")).toString() == QStringLiteral("Display P3")
                  && p3dci.value(QStringLiteral("note")).toString().contains(QStringLiteral("transfer")),
              "P3 primaries + 2.6 → Display P3 with a transfer note");
        const QVariantMap r1886 = mgr.nearestDisplay(xy(0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290), 2.4);
        check(r1886.value(QStringLiteral("display")).toString() == QStringLiteral("Rec.1886"), "Rec.709 + 2.4 → Rec.1886");
        const QVariantMap r2020 = mgr.nearestDisplay(xy(0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290), 2.4);
        check(r2020.value(QStringLiteral("display")).toString() == QStringLiteral("Rec.2020"), "BT.2020 → Rec.2020");
        const QVariantMap srgb = mgr.nearestDisplay(xy(0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290), 2.224);
        check(srgb.value(QStringLiteral("display")).toString() == QStringLiteral("sRGB")
                  && srgb.value(QStringLiteral("exact")).toBool(),
              "sRGB primaries + 2.22 → sRGB, exact");
    }

    std::printf("%s\n", g_failed ? "FAILED" : "PASS");
    return g_failed ? 1 : 0;
}
