// probe-ocio-pins — the per-clip scene chain's routing rules (colour plan
// stage 2), on OCIOConfigManager alone:
//   - every clip-side edit pins the clip on screen (with no clip, it sets
//     the starting chain); ↺ returns a slot to the default;
//   - dual view: every edit pins the focused side's clip; the B tab edits
//     B; "Copy A's chain to B" writes B's pins;
//   - the snapshot: per-side only when the sides differ; knee-only
//     differences keep the shader shared (sameShader).
//
// Usage:  probe-ocio-pins <config.ocio>

#include "color/colourspace_short_name.h"
#include "color/ocio_config_manager.h"

#include <QCoreApplication>

#include <cstdio>

using namespace qcv;

namespace {

int g_failed = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: probe-ocio-pins <config.ocio>\n");
        return 2;
    }
    OCIOConfigManager mgr;
    if (!mgr.loadConfigFile(QString::fromUtf8(argv[1]))) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    const QStringList cs = mgr.colorspaces();
    if (cs.size() < 4) {
        std::fprintf(stderr, "config has too few colourspaces\n");
        return 2;
    }
    const QString def = mgr.activeInput();
    // Three inputs different from the default and each other.
    QStringList picks;
    for (const QString &c : cs) {
        if (c != def && !picks.contains(c)) picks << c;
        if (picks.size() == 3) break;
    }
    const QString x = picks[0], y = picks[1], z = picks[2];

    // ---- Single view ----
    // No clip: the edit sets the starting chain every untouched clip shows.
    mgr.setViewContext("", false, "", "");
    mgr.setActiveInput(x);
    check(mgr.activeInput() == x, "no clip: an edit sets the starting chain");
    mgr.setViewContext("clip1", false, "clip1", "");
    check(mgr.activeInput() == x && !mgr.inputPinned(), "single: an untouched clip shows it");

    mgr.setActiveInput(y);
    check(mgr.inputPinned() && mgr.clipHasPins("clip1"), "single: an edit pins the clip");
    mgr.setViewContext("clip2", false, "clip2", "");
    check(mgr.activeInput() == x && !mgr.inputPinned(), "single: another clip is untouched");
    mgr.setViewContext("clip1", false, "clip1", "");
    check(mgr.activeInput() == y, "single: clip1 keeps its own Input");
    check(mgr.snapshot()->single.scene.input == y, "snapshot: single view draws clip1's Input");
    mgr.setSlotPinned("input", false);
    check(mgr.activeInput() == x && !mgr.clipHasPins("clip1"),
          "single: ↺ returns to the default and drops the empty pin");

    // ---- Dual view ----
    mgr.setViewContext("clipA", true, "clipA", "clipB");
    mgr.setActiveTab(0);
    mgr.setActiveInput(y);
    check(mgr.clipHasPins("clipA") && mgr.inputPinned(), "dual: an A edit pins clipA");
    mgr.setActiveTab(1);
    check(mgr.focusClipId() == "clipB" && mgr.activeInput() == x,
          "dual: the B tab shows clipB (still the default)");
    mgr.setActiveInput(z);
    check(mgr.clipHasPins("clipB"), "dual: a B edit pins clipB");
    {
        const auto snap = mgr.snapshot();
        check(snap->a.scene.input == y && snap->b.scene.input == z && snap->perSide(),
              "snapshot: sides differ → per-side chains");
    }
    check(mgr.specForClip("clipA").scene.input == y && mgr.specForClip("clipB").scene.input == z,
          "dual: pins are the clips' remembered state");

    mgr.copyAChainToB();
    check(mgr.specForClip("clipB").scene.input == y && !mgr.snapshot()->perSide(),
          "copy A → B: B's pins become A's chain");

    // Knee-only difference: two chains, one shader.
    mgr.setActiveTab(1);
    mgr.setKneeEnabled(true);
    mgr.setKneeSourceNits(4000);
    {
        const auto snap = mgr.snapshot();
        check(snap->perSide() && snap->a.sameShader(snap->b)
                  && snap->b.scene.kneeSourceNits == 4000.0f && !snap->a.scene.kneeEnabled,
              "knee on B only: per-side, same shader");
    }

    // Leaving dual: single view shows A's clip with its pins.
    mgr.setViewContext("clipA", false, "clipA", "clipB");
    check(mgr.focusClipId() == "clipA" && mgr.activeInput() == y,
          "single again: A's clip keeps its pin");

    // ---- Bulk (project panel) ----
    mgr.setViewContext("clipP", false, "clipP", "");
    mgr.setInputForClips({"c1", "c2", "c3"}, z);
    check(mgr.specForClip("c1").scene.input == z && mgr.specForClip("c3").scene.input == z,
          "bulk: Input for several clips");
    mgr.setActiveInput(y);   // clipP's own chain
    mgr.copyClipChain("clipP", {"c1", "c2"});
    check(mgr.specForClip("c1").scene.input == y && mgr.specForClip("c2").scene.input == y
              && mgr.specForClip("c3").scene.input == z,
          "bulk: Use Clip Chain of the clip on screen");
    mgr.resetClipChains({"c1", "c2", "c3"});
    check(!mgr.clipHasPins("c1") && !mgr.clipHasPins("c3")
              && mgr.specForClip("c1").scene.input == mgr.specForClip("").scene.input,
          "bulk: Reset Clip Chain");

    // ---- Badge names (the plan's table) ----
    using colourspace_names::shortName;
    const struct { const char *in, *out; } names[] = {
        {"ACEScg", "ACEScg"},
        {"ARRI LogC4", "ARRI LogC4"},
        {"ST2084-P3-D65 - Display", "ST2084-P3-D65"},
        {"Rec.1886 Rec.709 - Display", "Rec.1886 Rec.709"},
        {"Linear Rec.709", "Lin Rec.709"},
        {"Gamma 2.4 Encoded Rec.709", "Gamma 2.4 Rec.709"},
        {"Sony S-Log3 Venice S-Gamut3.Cine to ACES2065-1", "Sony S-Log3 Venice…"},
    };
    for (const auto &n : names) {
        const QString got = shortName(QString::fromUtf8(n.in));
        const QString what = QStringLiteral("badge: %1 → %2 (got %3)")
                                 .arg(QString::fromUtf8(n.in), QString::fromUtf8(n.out), got);
        check(got == QString::fromUtf8(n.out), qPrintable(what));
    }
    {
        // Collisions in the loaded config fall back to the full tidied name.
        const auto shorts = colourspace_names::shortNamesFor(mgr.colorspaces());
        QHash<QString, int> uses;
        for (const QString &v : shorts) uses[v] += 1;
        int collisions = 0;
        for (auto it = uses.constBegin(); it != uses.constEnd(); ++it)
            if (it.value() > 1) ++collisions;
        check(collisions == 0, "badge: no two colourspaces share a short name");
    }
    mgr.setViewContext("clipX", false, "clipX", "");
    QString linearIn = y;
    for (const QString &c : mgr.colorspaces())
        if (c.startsWith(QStringLiteral("Linear"))) { linearIn = c; break; }
    mgr.setActiveInput(linearIn);
    mgr.setActiveLook(mgr.looks().isEmpty() ? QString() : mgr.looks().first());
    {
        const auto shorts = colourspace_names::shortNamesFor(mgr.colorspaces());
        check(mgr.clipBadge("clipX").startsWith(shorts.value(linearIn))
                  && (mgr.looks().isEmpty() || mgr.clipBadge("clipX").endsWith(QStringLiteral("+ Look"))),
              qPrintable(QStringLiteral("badge: %1 → %2").arg(linearIn, mgr.clipBadge("clipX"))));
    }
    check(mgr.clipBadge("untouched").isEmpty(), "badge: none for an untouched clip");

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "all passed", g_failed);
    return g_failed ? 1 : 0;
}
