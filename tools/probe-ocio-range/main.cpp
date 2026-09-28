// probe-ocio-range — how the bundled OCIO chains treat values outside 0–1.
//
// The YUV→RGB converters hand OCIO the source's encoded RGB. Once they stop
// clamping, super-whites (> 1.0), sub-blacks (< 0.0) and out-of-gamut
// triplets (one channel negative) reach every Input decode. This sweeps
// every Input × Display × View of each config through the CPU processor
// (OCIO's reference path; the GPU shaders are built from the same ops) and
// reports, per Input:
//   - NaN/Inf: chains that produce a non-finite output for any probe value
//   - over:    chains where 1.09 still lands above 1.0's output (preserved)
//   - under:   chains where -0.07 still lands below 0.0's output (preserved)
//
// Usage:  probe-ocio-range <config.ocio> [<config.ocio> ...] [--verbose]

#include <OpenColorIO/OpenColorIO.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

namespace {

// 8-bit video-range extremes after level removal: Y=255 → 1.0913,
// Y=1 → -0.0685. Out-of-gamut triplets are what a saturated legal
// Y'CbCr colour turns into after the matrix.
struct Probe { const char *name; float rgb[3]; };
const Probe kProbes[] = {
    {"black",      { 0.0f,    0.0f,    0.0f  }},
    {"white",      { 1.0f,    1.0f,    1.0f  }},
    {"superwhite", { 1.0913f, 1.0913f, 1.0913f}},
    {"subblack",   {-0.0685f,-0.0685f,-0.0685f}},
    {"oog-red",    { 1.05f,  -0.05f,  -0.02f }},
    {"oog-blue",   {-0.03f,   0.40f,   1.10f }},
};
constexpr int kNumProbes = sizeof(kProbes) / sizeof(kProbes[0]);

float luma(const float *c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

bool finite3(const float *c)
{
    return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]);
}

struct InputStats {
    std::string name;
    int chains = 0, failed = 0, nonFinite = 0, over = 0, under = 0;
    std::vector<std::string> nanChains;
};

int sweep(const char *path, bool verbose)
{
    OCIO::ConstConfigRcPtr cfg;
    try {
        cfg = OCIO::Config::CreateFromFile(path);
    } catch (const OCIO::Exception &e) {
        std::fprintf(stderr, "%s: %s\n", path, e.what());
        return 1;
    }

    std::printf("\n=== %s ===\n", path);
    std::printf("%-44s %6s %5s %6s %6s %6s\n",
                "Input", "chains", "fail", "NaN", "over", "under");

    int totalNan = 0;
    for (int i = 0; i < cfg->getNumColorSpaces(); ++i) {
        const char *cs = cfg->getColorSpaceNameByIndex(i);
        auto space = cfg->getColorSpace(cs);
        if (!space || space->isData()) continue;

        InputStats st;
        st.name = cs;
        for (int d = 0; d < cfg->getNumDisplays(); ++d) {
            const char *display = cfg->getDisplay(d);
            for (int v = 0; v < cfg->getNumViews(display); ++v) {
                const char *view = cfg->getView(display, v);
                ++st.chains;
                float out[kNumProbes][3];
                try {
                    auto dvt = OCIO::DisplayViewTransform::Create();
                    dvt->setSrc(cs);
                    dvt->setDisplay(display);
                    dvt->setView(view);
                    auto cpu = cfg->getProcessor(dvt)->getDefaultCPUProcessor();
                    for (int p = 0; p < kNumProbes; ++p) {
                        std::memcpy(out[p], kProbes[p].rgb, sizeof(out[p]));
                        cpu->applyRGB(out[p]);
                    }
                } catch (const OCIO::Exception &) {
                    ++st.failed;
                    continue;
                }

                bool bad = false;
                for (int p = 0; p < kNumProbes; ++p) bad |= !finite3(out[p]);
                if (bad) {
                    ++st.nonFinite;
                    std::string chain = std::string(display) + " / " + view;
                    if (verbose) {
                        for (int p = 0; p < kNumProbes; ++p) {
                            if (!finite3(out[p])) chain += std::string(" [") + kProbes[p].name + "]";
                        }
                    }
                    st.nanChains.push_back(chain);
                    continue;
                }
                // Probe order: 0 black, 1 white, 2 superwhite, 3 subblack.
                if (luma(out[2]) > luma(out[1]) + 1e-5f) ++st.over;
                if (luma(out[3]) < luma(out[0]) - 1e-5f) ++st.under;
            }
        }
        totalNan += st.nonFinite;
        std::printf("%-44.44s %6d %5d %6d %6d %6d\n", st.name.c_str(),
                    st.chains, st.failed, st.nonFinite, st.over, st.under);
        const size_t shown = verbose ? st.nanChains.size()
                                     : std::min<size_t>(st.nanChains.size(), 3);
        for (size_t k = 0; k < shown; ++k) {
            std::printf("    NaN: %s\n", st.nanChains[k].c_str());
        }
        if (shown < st.nanChains.size()) {
            std::printf("    ... %zu more (--verbose)\n", st.nanChains.size() - shown);
        }
    }
    std::printf("non-finite chains in config: %d\n", totalNan);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    bool verbose = false;
    std::vector<const char *> paths;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
        else paths.push_back(argv[i]);
    }
    if (paths.empty()) {
        std::fprintf(stderr, "usage: probe-ocio-range <config.ocio> [...] [--verbose]\n");
        return 1;
    }
    int rc = 0;
    for (const char *p : paths) rc |= sweep(p, verbose);
    return rc;
}
