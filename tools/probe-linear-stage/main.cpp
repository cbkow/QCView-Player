// probe-linear-stage — CPU proof of the split OCIO chain and the knee.
//
// For every Input × Display × View of each config:
//   1. round trip: unsplit chain vs pre → linear stage at identity → post,
//      over a ramp of SDR and HDR values; reports the worst difference.
//   2. knee (with --knee): Rec.2100-PQ / ST2084-P3-D65 inputs through
//      SDR views, source peak 1000 nits → target 100: the output must be
//      monotonic, and 1000 nits must reach the display's white.
//
// Uses OCIOConfigManager + OcioChainBuilder exactly as the renderers do,
// and linear_stage::apply (the CPU twin of the MSL / HLSL stage).
//
// Usage:  probe-linear-stage <config.ocio> [...]

#include "color/linear_stage.h"
#include "color/ocio_chain_builder.h"
#include "color/ocio_config_manager.h"

#include <QCoreApplication>

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;
using namespace qcv;

namespace {

std::vector<std::array<float, 3>> probeValues()
{
    std::vector<std::array<float, 3>> v;
    for (float x : {0.0f, 0.001f, 0.01f, 0.05f, 0.18f, 0.5f, 0.75f, 1.0f, 1.5f, 4.0f}) {
        v.push_back({x, x, x});
        v.push_back({x, 0.5f * x, 0.25f * x});
        v.push_back({0.2f * x, x, 0.6f * x});
    }
    v.push_back({-0.05f, 0.2f, 0.4f});
    return v;
}

float relDiff(float a, float b)
{
    return std::abs(a - b) / std::max(1.0f, std::abs(b));
}

void runChain(OCIOConfigManager &mgr, OCIO::ConstConfigRcPtr cfg,
              const char *input, const char *display, const char *view,
              std::vector<std::pair<float, std::string>> &worstList,
              int &failed, int &nonFinite)
{
    mgr.setActiveInput(QString::fromUtf8(input));
    mgr.setActiveDisplay(QString::fromUtf8(display));
    mgr.setActiveView(QString::fromUtf8(view));

    OCIO::GroupTransformRcPtr whole =
        OcioChainBuilder::buildGroupTransform(mgr.focusedSpec(), cfg);
    OcioSplitTransforms split;
    if (!whole || !OcioChainBuilder::buildSplitTransforms(mgr.focusedSpec(), cfg, split)) {
        ++failed;
        return;
    }
    OCIO::ConstCPUProcessorRcPtr cw, cpre, cpost;
    try {
        cw    = cfg->getProcessor(whole)->getDefaultCPUProcessor();
        cpre  = cfg->getProcessor(split.pre)->getDefaultCPUProcessor();
        cpost = cfg->getProcessor(split.post)->getDefaultCPUProcessor();
    } catch (const OCIO::Exception &) {
        ++failed;
        return;
    }
    float worst = 0.0f;
    LinearStageSettings identity;
    const LinearStageGpu stage =
        linear_stage::resolve(identity, split.side, split.displayIsSdr);
    // Encoded inputs (video, log, PQ) only ever hold code values up to
    // 1.0; only linear inputs carry the HDR probe values.
    const std::string enc = cfg->getColorSpace(input)->getEncoding();
    const bool linearIn = enc == "scene-linear" || enc == "display-linear";
    for (const auto &p : probeValues()) {
        if (!linearIn && std::max(p[0], std::max(p[1], p[2])) > 1.0f) continue;
        float a[3] = {p[0], p[1], p[2]};
        float b[3] = {p[0], p[1], p[2]};
        cw->applyRGB(a);
        cpre->applyRGB(b);
        linear_stage::apply(b, stage);
        cpost->applyRGB(b);
        if (const char *dbg = std::getenv("PROBE_CHAIN");
            dbg && std::string(dbg) == std::string(input) + "|" + display + "|" + view) {
            std::printf("  in %8.4f %8.4f %8.4f  unsplit %12.5g %12.5g %12.5g  split %12.5g %12.5g %12.5g\n",
                        p[0], p[1], p[2], a[0], a[1], a[2], b[0], b[1], b[2]);
        }
        // Outputs above 100 (10,000 nits, the top of PQ) come only from
        // log codes far outside their curve's domain; float32 rounding
        // leaks between channels there. Not a meaningful comparison.
        if (std::abs(a[0]) > 100.0f || std::abs(a[1]) > 100.0f || std::abs(a[2]) > 100.0f) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(a[c]) || !std::isfinite(b[c])) {
                if (std::isfinite(a[c]) != std::isfinite(b[c])) ++nonFinite;
                continue;
            }
            const float d = relDiff(b[c], a[c]);
            worst = std::max(worst, d);
        }
    }
    worstList.emplace_back(worst, std::string(input) + " | " + display + " / " + view);
}

// PQ code value for `nits`, as an RGB triple (neutral).
float pqOf(float nits) { return linear_stage::pqEncode(nits / 10000.0f); }

void kneeCheck(OCIOConfigManager &mgr, OCIO::ConstConfigRcPtr cfg,
               const char *input, const char *display, const char *view)
{
    mgr.setActiveInput(QString::fromUtf8(input));
    mgr.setActiveDisplay(QString::fromUtf8(display));
    mgr.setActiveView(QString::fromUtf8(view));
    OcioSplitTransforms split;
    QString err;
    if (!OcioChainBuilder::buildSplitTransforms(mgr.focusedSpec(), cfg, split, &err)) {
        std::printf("  knee %s → %s/%s: split failed: %s\n", input, display, view,
                    qPrintable(err));
        return;
    }
    auto cpre  = cfg->getProcessor(split.pre)->getDefaultCPUProcessor();
    auto cpost = cfg->getProcessor(split.post)->getDefaultCPUProcessor();
    LinearStageSettings s;
    s.kneeEnabled    = true;
    s.kneeSourceNits = 1000.0f;
    const LinearStageGpu on  = linear_stage::resolve(s, split.side, split.displayIsSdr);
    const LinearStageGpu off = linear_stage::resolve(LinearStageSettings{}, split.side,
                                                     split.displayIsSdr);
    const float ksNits = linear_stage::kneeStartNits(on.p1[0], s.kneeSourceNits);

    auto out = [&](float nits, const LinearStageGpu &g) {
        float v[3] = {pqOf(nits), pqOf(nits), pqOf(nits)};
        cpre->applyRGB(v);
        linear_stage::apply(v, g);
        cpost->applyRGB(v);
        return v[1];
    };
    bool monotonic = true;
    float prev = -1.0f;
    for (float nits = 1.0f; nits <= 1200.0f; nits *= 1.05f) {
        const float y = out(nits, on);
        if (y < prev - 1e-6f) monotonic = false;
        prev = y;
    }
    std::printf("  knee %-24s → %s / %s  (%s side, %s)\n", input, display, view,
                split.side == InterchangeSide::Scene ? "scene" : "display",
                split.displayIsSdr ? "SDR" : "HDR");
    std::printf("    knee start %.1f nits, monotonic %s\n", ksNits, monotonic ? "yes" : "NO");
    for (float nits : {10.0f, 18.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 4000.0f}) {
        std::printf("    %6.0f nits: off %.4f  on %.4f\n", nits, out(nits, off), out(nits, on));
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) paths.emplace_back(argv[i]);
    if (paths.empty()) {
        std::fprintf(stderr, "usage: probe-linear-stage <config.ocio> [...]\n");
        return 1;
    }
    for (const auto &path : paths) {
        OCIOConfigManager mgr;
        if (!mgr.loadConfigFile(QString::fromStdString(path))) return 1;
        OCIO::ConstConfigRcPtr cfg = OCIO::Config::CreateFromFile(path.c_str());

        std::vector<std::pair<float, std::string>> worstList;
        int chains = 0, failed = 0, nonFinite = 0;
        for (int i = 0; i < cfg->getNumColorSpaces(); ++i) {
            const char *cs = cfg->getColorSpaceNameByIndex(i);
            if (cfg->getColorSpace(cs)->isData()) continue;
            for (int d = 0; d < cfg->getNumDisplays(); ++d) {
                const char *display = cfg->getDisplay(d);
                for (int v = 0; v < cfg->getNumViews(display); ++v) {
                    ++chains;
                    runChain(mgr, cfg, cs, display, cfg->getView(display, v),
                             worstList, failed, nonFinite);
                }
            }
        }
        std::printf("\n=== %s ===\n", path.c_str());
        std::sort(worstList.begin(), worstList.end(),
                  [](const auto &x, const auto &y) { return x.first > y.first; });
        std::printf("round trip: %d chains, %d not split (data view / no role), "
                    "%d finite-mismatch\n", chains, failed, nonFinite);
        for (size_t k = 0; k < std::min<size_t>(4, worstList.size()); ++k) {
            std::printf("  worst rel diff %.2e  %s\n", worstList[k].first,
                        worstList[k].second.c_str());
        }

        const char *sdrDisplay = cfg->getNumDisplays() > 0 ? cfg->getDisplay(0) : "";
        for (const char *in : {"Rec.2100-PQ", "Rec.2100-PQ - Display",
                               "ST2084-P3-D65", "ST2084-P3-D65 - Display"}) {
            if (!cfg->getColorSpace(in)) continue;
            for (int v = 0; v < cfg->getNumViews(sdrDisplay); ++v) {
                const char *view = cfg->getView(sdrDisplay, v);
                const std::string vn(view);
                if (vn.find("Standard") != std::string::npos
                    || vn.find("Un-tone-mapped") != std::string::npos) {
                    kneeCheck(mgr, cfg, in, sdrDisplay, view);
                }
            }
        }
    }
    return 0;
}
