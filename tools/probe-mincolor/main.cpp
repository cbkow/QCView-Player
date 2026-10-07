// probe-mincolor — the minColor engine on the CPU twin: a few known
// values through each chain shape, printed, so the GPU kernel (and the
// After Effects effects) can be compared number for number.
//
//   probe-mincolor            prints the table
//   probe-mincolor r g b      one linear Rec.709-encoded value through every shape

#include "color/mincolor_chain.h"

#include <cstdio>
#include <cstdlib>

using namespace qcv;

namespace {

void run(const char *name, const MinColorChain &chain, const float *in, bool sdrCapture = false,
         bool edr = false)
{
    const MinColorGpu g = mincolor::resolve(chain, sdrCapture, edr);
    float rgb[3] = {in[0], in[1], in[2]};
    mincolor::apply(g, rgb);
    std::printf("  %-34s %7.4f %7.4f %7.4f  ->  %7.4f %7.4f %7.4f\n", name, in[0], in[1], in[2],
                rgb[0], rgb[1], rgb[2]);
}

} // namespace

int main(int argc, char **argv)
{
    float custom[3] = {1.0f, 1.0f, 1.0f};
    const bool haveCustom = argc >= 4;
    if (haveCustom) {
        for (int i = 0; i < 3; ++i) custom[i] = static_cast<float>(std::atof(argv[i + 1]));
    }
    const float white[3] = {1.0f, 1.0f, 1.0f};
    const float grey[3]  = {0.18f, 0.18f, 0.18f};
    const float red[3]   = {1.0f, 0.0f, 0.0f};
    const float hot[3]   = {8.0f, 8.0f, 8.0f};

    MinColorChain base;
    base.input.gamut    = DRT_IN_REC709;
    base.input.transfer = DRT_OETF_LINEAR;   // the values below are linear Rec.709
    base.output.display = 1;                 // sRGB Display - 2.2 Power / Rec.709

    MinColorChain untoned = base;
    MinColorChain opendrt = base; opendrt.output.openDrt = true;
    MinColorChain agx = base;     agx.agx.enabled = true;
    MinColorChain knee = base;    knee.knee.enabled = true; knee.knee.sourceNits = 1000.0f; knee.knee.targetNits = 100.0f;
    MinColorChain pq = opendrt;   pq.output.display = 6; pq.output.peakNits = 1000.0f;   // Rec.2100 PQ
    MinColorChain edr = opendrt;  edr.output.display = mincolor::linearDisplayIndex(DRT_DG_REC709); edr.output.peakNits = 1000.0f;
    MinColorChain p3 = untoned;   p3.output.display = mincolor::linearDisplayIndex(DRT_DG_P3D65);   // QCView extra: macOS EDR P3
    MinColorChain p3drt = opendrt; p3drt.output.display = p3.output.display; p3drt.output.peakNits = 1000.0f;

    std::printf("minColor engine, CPU twin (linear Rec.709 in -> display code out)\n");
    const float *inputs[] = {white, grey, red, hot};
    const char *names[] = {"white", "grey 0.18", "red", "8.0 (hot)"};
    for (int i = 0; i < 4; ++i) {
        const float *v = haveCustom ? custom : inputs[i];
        std::printf("%s\n", haveCustom ? "custom" : names[i]);
        run("un-tone-mapped sRGB", untoned, v);
        run("OpenDRT Standard, sRGB 100 nits", opendrt, v);
        run("AgX defaults, sRGB", agx, v);
        run("knee 1000->100, un-tone-mapped", knee, v);
        run("OpenDRT, Rec.2100 PQ 1000 nits", pq, v);
        run("OpenDRT, linear 709 1000 nits EDR", edr, v, false, true);
        run("OpenDRT, SDR capture of PQ chain", pq, v, true, false);
        run("un-tone-mapped, linear P3-D65 EDR", p3, v, false, true);
        run("OpenDRT, linear P3 1000 nits EDR", p3drt, v, false, true);
        run("un-tone-mapped, SDR capture of P3", p3, v, true, false);
        if (haveCustom) break;
    }
    return 0;
}
