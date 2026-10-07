// MinColorChain — the second colour engine's chain, as a value, and the
// GPU blocks it resolves to.
//
// minColor is QCView's OCIO-free engine: the minColorAE core (vendored in
// mincolor/, see VENDORED.md) run as one kernel per platform compiled once,
// every change a uniform update. The chain has the shape of minColorAE's
// effect stack, so a comp in After Effects and the same frame here go
// through the same maths in the same order:
//
//   Input → Knee → AgX → Rendering (un-tone-mapped | OpenDRT) → Display
//   clip    clip    view   view                                  view
//
// Between the steps: linear Rec.2020, 1.0 = 100 nits (the knee's and the
// scopes' space). AgX and OpenDRT are both picture formations and cannot
// stack: AgX on forces the Rendering to un-tone-mapped, so only the
// Display encode follows it — exactly how the two effects coexist in AE.
//
// Nothing here applies itself: the one On / Off switch (OCIOConfigManager::
// engaged) bypasses both engines, and the engine selector only decides
// what On runs. minColor is never a fallback for a missing OCIO config.
//
// The GPU block is the vendored core's own flat 4-byte-scalar structs
// (DrtParams for the Input half and the Output half, DrtAgxParams for
// AgX) plus a small flags block, so the bytes resolve() fills upload
// unchanged to MSL and HLSL. resolve() is host C++: derive runs once per
// change, the kernel only reads.

#pragma once

#include "mincolor/opendrt.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <cstring>
#include <optional>

namespace qcv {

// The Input half: what the file is. Per clip (a pin, else the default).
struct MinColorInput {
    int  gamut    = DRT_IN_REC709;     // DRT_IN_*
    int  transfer = DRT_OETF_REC1886;  // DRT_OETF_* (no inverse entries)
    bool limited  = false;             // limited (video) range codes

    bool operator==(const MinColorInput &o) const
    {
        return gamut == o.gamut && transfer == o.transfer && limited == o.limited;
    }
    bool operator!=(const MinColorInput &o) const { return !(*this == o); }
};

// The Knee: QCView's Highlight Knee (the same maths the OCIO stage runs —
// minColorAE ported it from here). Per clip.
struct MinColorKnee {
    bool  enabled    = false;
    float sourceNits = 1000.0f;
    float targetNits = 100.0f;
    float start      = -1.0f;   // fraction of the target in PQ; < 0 = BT.2390's

    bool operator==(const MinColorKnee &o) const
    {
        return enabled == o.enabled && sourceNits == o.sourceNits
            && targetNits == o.targetNits && start == o.start;
    }
    bool operator!=(const MinColorKnee &o) const { return !(*this == o); }
};

// AgX: minColor's parametric, HDR-capable AgX (Blender-compatible at its
// defaults). Shared by the view. `params` holds the user fields of
// DrtAgxParams; resolve() derives the rest.
struct MinColorAgx {
    bool  enabled = false;
    int   target  = 1;         // 0 Rec.2020, 1 Rec.709, 2 P3-D65
    float peak    = 100.0f;    // nits
    float whiteEv = 6.5f;
    float blackEv = -10.0f;
    float contrast = 2.4f;
    float toe      = 1.5f;
    float shoulder = 1.5f;
    float hueRestore = 0.6f;
    float hdrPurity  = 0.5f;
    float outset     = 1.0f;

    bool operator==(const MinColorAgx &o) const
    {
        return enabled == o.enabled && target == o.target && peak == o.peak
            && whiteEv == o.whiteEv && blackEv == o.blackEv && contrast == o.contrast
            && toe == o.toe && shoulder == o.shoulder && hueRestore == o.hueRestore
            && hdrPurity == o.hdrPurity && outset == o.outset;
    }
    bool operator!=(const MinColorAgx &o) const { return !(*this == o); }
};

// Rendering + Display: the Output half. Shared by the view.
struct MinColorOutput {
    bool  openDrt   = false;   // false = un-tone-mapped (a plain conversion)
    int   look      = 0;       // drt::kLooks index
    int   tonescale = 0;       // 0 = the look's own, 1..13 = drt::kTonescales[n-1]
    int   cwp       = 0;       // 0 = the look's creative white, 1..6 = drt::kCwpNames[n-1]
    float cwpLimit  = 0.25f;
    int   display   = 1;       // drt::kDisplays index (1 = sRGB Display - 2.2 Power / Rec.709)
    int   surround  = DRT_SURROUND_DARK;
    float peakNits  = 100.0f;  // tn_Lp: the display's peak luminance
    float greyBoost = 0.13f;   // tn_gb, HDR grey boost
    float hdrPurity = 0.5f;    // pt_hdr
    float greyNits  = 10.0f;   // tn_Lg

    bool operator==(const MinColorOutput &o) const
    {
        return openDrt == o.openDrt && look == o.look && tonescale == o.tonescale
            && cwp == o.cwp && cwpLimit == o.cwpLimit && display == o.display
            && surround == o.surround && peakNits == o.peakNits && greyBoost == o.greyBoost
            && hdrPurity == o.hdrPurity && greyNits == o.greyNits;
    }
    bool operator!=(const MinColorOutput &o) const { return !(*this == o); }
};

// A clip's pins: the Input and / or Knee it keeps regardless of the
// default (the OCIO engine's OcioScenePin, for the two clip-side steps).
struct MinColorPin {
    std::optional<MinColorInput> input;
    std::optional<MinColorKnee>  knee;
    bool empty() const { return !input && !knee; }
    QVariantMap toVariant() const;
    static MinColorPin fromVariant(const QVariantMap &m);
};

// One side's whole chain, resolved (the clip's Input and Knee, the view's
// AgX and Output).
struct MinColorChain {
    MinColorInput  input;
    MinColorKnee   knee;
    MinColorAgx    agx;
    MinColorOutput output;

    bool operator==(const MinColorChain &o) const
    {
        return input == o.input && knee == o.knee && agx == o.agx && output == o.output;
    }
    bool operator!=(const MinColorChain &o) const { return !(*this == o); }

    QVariantMap toVariant() const;
    static MinColorChain fromVariant(const QVariantMap &m);
};

// The flags block: all 4-byte scalars, a multiple of 4 (one float4).
struct MinColorFlagsGpu {
    int   kneeOn  = 0;
    int   agxOn   = 0;
    float outScale = 1.0f;   // multiplies the kernel's output: EDR = peak / 100
    int   pad     = 0;
};

// What the kernels bind: the vendored structs, derived, plus the flags.
// Identical bytes on C++, MSL and HLSL (see opendrt_params.h "WHY THIS
// SHAPE").
struct MinColorGpu {
    drt::DrtParams   in{};     // Input half: in_gamut / in_oetf / in_range → working gamut (Rec.2020); knee fields
    drt::DrtParams   out{};    // Output half: in = linear Rec.2020 (or AgX's target), look, display, out_view
    drt::DrtAgxParams agx{};
    MinColorFlagsGpu flags{};

    bool operator==(const MinColorGpu &o) const
    {
        return std::memcmp(this, &o, sizeof(MinColorGpu)) == 0;
    }
    bool operator!=(const MinColorGpu &o) const { return !(*this == o); }
};
static_assert(sizeof(MinColorGpu) == 2 * sizeof(drt::DrtParams) + sizeof(drt::DrtAgxParams) + 16,
              "MinColorGpu must stay four flat blocks of 4-byte scalars");

namespace mincolor {

// Resolve a chain into its GPU blocks. `sdrCapture`: build the sRGB
// display encoding at peak 100 instead of the chain's Display, for
// screenshots and note thumbnails (the OCIO capture path's rule).
// `edrLinear`: the swapchain is display-linear with 1.0 = 100 nits
// (macOS EDR); the chain's Display must then be a linear hand-off and the
// kernel's output is scaled by peak / 100 (flags.outScale).
MinColorGpu resolve(const MinColorChain &chain, bool sdrCapture, bool edrLinear);

// CPU reference of the whole chain on one pixel (linear RGB in the
// Input's encoding → display-encoded RGB). LUT export and tests.
void apply(const MinColorGpu &g, float *rgb);

// Display preset index whose EOTF is a linear hand-off in `gamut`
// (DRT_DG_WORKING / REC709 / …), or -1.
int linearDisplayIndex(int displayGamut);

// QCView's own display entries after the vendored drt::kDisplays: the
// macOS EDR P3 swapchain wants linear P3-D65, which upstream's table
// has no hand-off for. Indices continue from drt::kDisplayCount.
struct ExtraDisplay { const char *name; int eotf; int displayGamut; };
inline constexpr ExtraDisplay kExtraDisplays[] = {
    {"None - Linear / P3-D65", DRT_EOTF_LINEAR, DRT_DG_P3D65},
};
inline constexpr int kExtraDisplayCount = 1;
inline constexpr int kDisplayTotal = drt::kDisplayCount + kExtraDisplayCount;
// EOTF / gamut of any display index, vendored or extra.
int displayEotf(int index);
int displayGamut(int index);

// Name tables for the panel reels (drt::k* tables, by index).
QStringList inputGamutNames();
QStringList inputTransferNames();    // the non-inverse entries only
QStringList lookNames();
QStringList tonescaleNames();        // "Use look" first
QStringList creativeWhiteNames();    // "Use look" first
QStringList displayNames();

} // namespace mincolor

} // namespace qcv
