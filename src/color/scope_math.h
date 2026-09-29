// Vectorscope math — shared by the Metal / D3D11 scope renderers, the
// GUI-side graticule geometry and the probe tools.
//
// Each tapped pixel becomes one count in a kScopeGrid² histogram:
//   vectorscope — (Cb, Cr), Tek / Resolve orientation: Cb horizontal
//                 (+ right), Cr vertical (+ up), the ±0.5 range filling
//                 the grid at zoom 1;
//   waveform    — (image column, luma level): Y′ of the same scope-space
//                 encoding, SDR / Signal levels −10 %…110 % (so sub-blacks
//                 and super-whites show), HDR levels as linear luminance
//                 in nits, 0…peak (300 / 600 / 1 000 / 2 000 / 4 000) —
//                 levels above it pile up at the top, tinted red. Linear,
//                 not PQ: judging nits matters more than perceptual spacing.
//   peaks       — (waveform only) a second pass over every source pixel,
//                 not the tap: the highest waveform level (nits / Y′) and
//                 the brightest channel, per side (ScopePeaks).
//
// Tiers (labelled on the scope, see WindowManager's scope state):
//   Signal   — nothing known: Y'CbCr straight from the source's encoded
//              RGB with the file's matrix (a nominal 1/2.4 curve first
//              for linear RGB sources). No colour targets.
//   Assumed  — file tags / format rules name a colourspace.
//   Input    — the user's OCIO Input.
// For Assumed / Input, OCIO converts the colourspace to the interchange
// role on its own side ("OCIOScope"), the linear_stage matrices take it
// to linear Rec.2020 (1.0 = 100 nits), and then:
//   SDR scale — Rec.709 primaries, pure 2.4 (BT.1886, black 0) inverse,
//               709 matrix;
//   HDR scale — Rec.2020 primaries, PQ, 2020 matrix.
// Curves are sign-preserving so out-of-gamut values plot outside the
// hexagon (and are counted separately for the warning tint) instead of
// clamping or turning into NaN.

#pragma once

#include "linear_stage.h"

#include <QString>

#include <algorithm>
#include <cmath>

namespace qcv {

constexpr int kScopeGrid        = 512;   // bins per axis
constexpr int kScopeCopies      = 4;     // grid copies per side (atomic contention)
constexpr int kScopeMaxTapWidth = 960;   // tap resolution cap

enum class ScopeTier  : int { Signal = 0, Assumed = 1, Input = 2 };
enum class ScopeKind  : int { Vectorscope = 0, Waveform = 1 };
enum class ScopeScale : int { Sdr = 0, Hdr = 1 };

// GUI → renderer.
struct ScopeConfig {
    ScopeKind  kind = ScopeKind::Vectorscope;
    bool       active = false;
    ScopeTier  tier = ScopeTier::Signal;
    QString    colorspace;               // Assumed / Input: the resolved colourspace
    QString    configPath;               // "" = the live config; else the built-in
                                         // fallback that names `colorspace`
    ScopeScale scale = ScopeScale::Sdr;
    int        signalMatrix = 1;         // Signal tier: 0 = BT.601, 1 = BT.709, 2 = BT.2020
    bool       signalNominalCurve = false; // Signal tier, linear RGB source
    int        zoom = 1;                 // 1, 2, 4
    float      brightness = 1.0f;        // trace exposure k
    bool       colorize = false;
    bool       dual = false;             // A + B overlay (cyan / orange)
    int        waveformPeakNits = 1000;  // waveform HDR scale: top of the face, nits
    quint32    peakEpoch = 0;            // ScopeController's peak reset count, echoed
                                         // back in ScopePeaks (drops in-flight images)

    // Dual: side B's own interpretation (its tags can differ from A's —
    // an SDR B beside a PQ A). The scale stays shared.
    ScopeTier  tierB = ScopeTier::Signal;
    QString    colorspaceB;
    QString    configPathB;
    int        signalMatrixB = 1;
    bool       signalNominalCurveB = false;

    // This config as seen by one side (0 = A, 1 = B): B's interpretation
    // moved into the main fields.
    ScopeConfig forSide(int side) const
    {
        ScopeConfig c = *this;
        if (side == 1) {
            c.tier = tierB;
            c.colorspace = colorspaceB;
            c.configPath = configPathB;
            c.signalMatrix = signalMatrixB;
            c.signalNominalCurve = signalNominalCurveB;
        }
        return c;
    }
};

// Accumulate uniforms (all float4 — identical layout in C++ / MSL / HLSL).
struct ScopeAccumGpu {
    float to0[4], to1[4], to2[4];   // interchange → linear Rec.2020
    float p0[4];   // x = converted (0/1), y = scale (0 SDR, 1 HDR), z = zoom, w = signal matrix
    float p1[4];   // x = nominal curve (0/1), y = tap width, z = tap height, w = side (0 A, 1 B)
    float p2[4];   // x = kind (0 vectorscope, 1 waveform), y = waveform HDR top (nits),
                   // z = pass (0 accumulate, 1 peak), w unused
};

// Waveform peaks of the newest image, per side: the waveform's level
// (nits on the HDR scale, Y′ 0..1 on SDR) and the brightest channel
// (linear Rec.2020 nits / encoded 0..1), over every source pixel.
struct ScopePeaks {
    int   sides = 0;          // layout: 0 = none, 1, or 2 (dual)
    bool  measured[2] = {false, false};   // false = that side in a timeline gap
    quint64 frameStamp = 0;   // renderer's displayed-frame count when measured
    quint32 epoch = 0;        // the config's peakEpoch it was measured under
    bool  hdr = false;
    float level[2]   = {0.0f, 0.0f};
    float channel[2] = {0.0f, 0.0f};
};

// Draw uniforms.
struct ScopeDrawGpu {
    float p0[4];   // x = count gain, y = colourize (0/1), z unused, w = sides (1/2)
    float p1[4];   // x = zoom, y = kind, zw unused
};

namespace scope_math {

// Waveform level range (bottom, top) of the grid for a scale.
inline void waveformRange(bool hdr, float peakNits, float &lo, float &hi)
{
    if (hdr) { lo = 0.0f; hi = peakNits; }   // linear luminance, nits
    else     { lo = -0.1f; hi = 1.1f; }      // Y′ with sub-black / super-white room
}

// Luma coefficients (Kr, Kb) for matrix 0 = BT.601, 1 = BT.709, 2 = BT.2020.
inline void lumaCoefficients(int matrix, float &kr, float &kb)
{
    if (matrix == 2)      { kr = 0.2627f; kb = 0.0593f; }
    else if (matrix == 0) { kr = 0.299f;  kb = 0.114f;  }
    else                  { kr = 0.2126f; kb = 0.0722f; }
}

inline void ycc(int matrix, const float *rgb, float &y, float &cb, float &cr)
{
    float kr, kb;
    lumaCoefficients(matrix, kr, kb);
    y  = kr * rgb[0] + (1.0f - kr - kb) * rgb[1] + kb * rgb[2];
    cb = (rgb[2] - y) / (2.0f * (1.0f - kb));
    cr = (rgb[0] - y) / (2.0f * (1.0f - kr));
}

inline float spow(float x, float k) { return std::copysign(std::pow(std::abs(x), k), x); }

inline ScopeAccumGpu resolveAccum(const ScopeConfig &c, InterchangeSide side,
                                  bool converted, int tapW, int tapH, int sideIndex)
{
    ScopeAccumGpu g{};
    const LinearStageGpu m = linear_stage::resolve(LinearStageSettings{}, side, true);
    std::copy(m.to0, m.to0 + 4, g.to0);
    std::copy(m.to1, m.to1 + 4, g.to1);
    std::copy(m.to2, m.to2 + 4, g.to2);
    g.p0[0] = converted ? 1.0f : 0.0f;
    g.p0[1] = c.scale == ScopeScale::Hdr ? 1.0f : 0.0f;
    g.p0[2] = static_cast<float>(std::clamp(c.zoom, 1, 8));
    g.p0[3] = static_cast<float>(c.signalMatrix);
    g.p1[0] = c.signalNominalCurve ? 1.0f : 0.0f;
    g.p1[1] = static_cast<float>(tapW);
    g.p1[2] = static_cast<float>(tapH);
    g.p1[3] = static_cast<float>(sideIndex);
    g.p2[0] = c.kind == ScopeKind::Waveform ? 1.0f : 0.0f;
    g.p2[1] = static_cast<float>(std::clamp(c.waveformPeakNits, 100, 10000));
    return g;
}

inline ScopeDrawGpu resolveDraw(const ScopeConfig &c, int pixelsSampled)
{
    ScopeDrawGpu g{};
    // Vectorscope: a bin holding 1/4096 of the sampled pixels reaches
    // 1 − 1/e at brightness 1 — independent of the tap size. Waveform:
    // counts spread over columns, so a bin holding 1/32 of a column's
    // samples (1/16384 of all) does.
    const float per = c.kind == ScopeKind::Waveform ? 16384.0f : 4096.0f;
    g.p0[0] = std::max(c.brightness, 0.01f) * per / std::max(pixelsSampled, 1);
    g.p1[1] = c.kind == ScopeKind::Waveform ? 1.0f : 0.0f;
    g.p0[1] = c.colorize ? 1.0f : 0.0f;
    g.p0[3] = c.dual ? 2.0f : 1.0f;
    g.p1[0] = static_cast<float>(std::clamp(c.zoom, 1, 8));
    return g;
}

// Rec.2020 → Rec.709, both linear D65.
inline void rec2020To709(const float *in, float *out)
{
    out[0] =  1.6604910f * in[0] - 0.5876411f * in[1] - 0.0728499f * in[2];
    out[1] = -0.1245505f * in[0] + 1.1328999f * in[1] - 0.0083494f * in[2];
    out[2] = -0.0181508f * in[0] - 0.1005789f * in[1] + 1.1187297f * in[2];
}

// One pixel in scope space: (Y′, Cb, Cr), whether it is outside the
// scale's gamut, HDR luminance in nits (converted HDR only) and the
// brightest channel (linear nits on the HDR scale, else encoded 0..1).
// `rgb` is the source's encoded RGB (Signal) or the OCIOScope output in
// the interchange space (converted). Mirrored by qs_classify in the
// kernels.
struct ScopePixel {
    float y = 0.0f, cb = 0.0f, cr = 0.0f;
    bool  oog = false;
    float nits = 0.0f;
    float channel = 0.0f;
};

inline ScopePixel classify(const ScopeAccumGpu &g, const float *rgbIn)
{
    ScopePixel p;
    if (g.p0[0] > 0.5f) {
        float l[3];
        for (int i = 0; i < 3; ++i) {
            const float *row = i == 0 ? g.to0 : (i == 1 ? g.to1 : g.to2);
            l[i] = row[0] * rgbIn[0] + row[1] * rgbIn[1] + row[2] * rgbIn[2];
        }
        float e[3];
        if (g.p0[1] < 0.5f) {
            float l7[3];
            rec2020To709(l, l7);
            p.oog = l7[0] < -0.002f || l7[1] < -0.002f || l7[2] < -0.002f;
            for (int i = 0; i < 3; ++i) e[i] = spow(l7[i], 1.0f / 2.4f);
            p.channel = std::max({e[0], e[1], e[2]});
            ycc(1, e, p.y, p.cb, p.cr);
        } else {
            p.oog = l[0] < -0.002f || l[1] < -0.002f || l[2] < -0.002f;
            p.nits = 100.0f * (0.2627f * l[0] + 0.6780f * l[1] + 0.0593f * l[2]);
            p.channel = 100.0f * std::max({l[0], l[1], l[2]});
            for (int i = 0; i < 3; ++i) {
                e[i] = std::copysign(linear_stage::pqEncode(std::abs(l[i]) * 0.01f), l[i]);
            }
            ycc(2, e, p.y, p.cb, p.cr);
        }
    } else {
        float e[3] = {rgbIn[0], rgbIn[1], rgbIn[2]};
        if (g.p1[0] > 0.5f) {
            for (int i = 0; i < 3; ++i) e[i] = spow(e[i], 1.0f / 2.4f);
        }
        p.channel = std::max({e[0], e[1], e[2]});
        ycc(static_cast<int>(g.p0[3]), e, p.y, p.cb, p.cr);
    }
    return p;
}

// The waveform's level for a pixel (peak pass): nits on the HDR scale,
// else Y′.
inline float waveformLevel(const ScopeAccumGpu &g, const ScopePixel &p)
{
    return g.p0[0] > 0.5f && g.p0[1] > 0.5f ? p.nits : p.y;
}

// The bin for one pixel, and whether it is outside the scale's gamut /
// range. `col01` = the pixel's column position 0..1 (waveform only).
inline void bin(const ScopeAccumGpu &g, const float *rgbIn, int &bx, int &by, bool &oog,
                float col01 = 0.0f)
{
    const ScopePixel p = classify(g, rgbIn);
    float y = p.y;
    const float cb = p.cb, cr = p.cr, nits = p.nits;
    oog = p.oog;
    if (g.p2[0] > 0.5f) {
        // Waveform: the column, and the level in the scale's range — SDR /
        // Signal Y′, HDR luminance in nits. SDR levels outside 0..1, HDR
        // levels above the peak count as out of range (the red tint).
        const bool hdr = g.p0[0] > 0.5f && g.p0[1] > 0.5f;
        float lo, hi;
        waveformRange(hdr, g.p2[1], lo, hi);
        if (hdr) y = nits;
        oog = hdr ? y > hi + 1e-3f : (y > 1.0f + 1e-4f || y < -1e-4f);
        bx = std::clamp(static_cast<int>(std::floor(col01 * kScopeGrid)), 0, kScopeGrid - 1);
        by = std::clamp(static_cast<int>(std::floor((hi - y) / (hi - lo) * kScopeGrid)), 0,
                        kScopeGrid - 1);
        return;
    }
    const float zoom = g.p0[2];
    const float fx = (cb * zoom + 0.5f) * kScopeGrid;
    const float fy = (0.5f - cr * zoom) * kScopeGrid;
    bx = std::clamp(static_cast<int>(std::floor(fx)), 0, kScopeGrid - 1);
    by = std::clamp(static_cast<int>(std::floor(fy)), 0, kScopeGrid - 1);
}

// Graticule helper: where a scale-space colour lands, as (x, y) in 0..1
// of the scope (same mapping as the kernel). `lin2020` is linear
// Rec.2020, 1.0 = 100 nits.
inline void targetPoint(const float *lin2020, ScopeScale scale, int zoom, float &x, float &y)
{
    float e[3], yy, cb, cr;
    if (scale == ScopeScale::Sdr) {
        float l7[3];
        rec2020To709(lin2020, l7);
        for (int i = 0; i < 3; ++i) e[i] = spow(l7[i], 1.0f / 2.4f);
        ycc(1, e, yy, cb, cr);
    } else {
        for (int i = 0; i < 3; ++i) {
            e[i] = std::copysign(linear_stage::pqEncode(std::abs(lin2020[i]) * 0.01f), lin2020[i]);
        }
        ycc(2, e, yy, cb, cr);
    }
    x = cb * zoom + 0.5f;
    y = 0.5f - cr * zoom;
}

} // namespace scope_math

} // namespace qcv
