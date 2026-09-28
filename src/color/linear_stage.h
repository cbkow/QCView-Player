// LinearStage — the one linear-light step between the two halves of the
// split OCIO chain (see OcioChainBuilder::buildSplit):
//
//   Input → Look → Scene LUT → [to interchange] → gain → knee → [back]
//         → Display/View → Display LUT
//
// This is the slot OCIO's own LegacyViewingPipeline calls linearCC and
// Blender's viewport fills with exposure + curves between its "to scene
// linear" and "scene linear to display" processors. Ours holds:
//   - gain: the Brightness / Exposure multiplier (1.0 = identity)
//   - knee: a BT.2390-style Hermite shoulder applied in PQ space to
//     max(R,G,B), with all three channels scaled by the same ratio so hue
//     holds. Compresses [knee start, source peak] onto [knee start,
//     target peak]; values at or above the source peak land on the target.
//
// The stage runs in linear Rec.2020 — the fixed matrices below map each
// interchange role to it and back. Both interchange spaces put 1.0 at
// 100 nits (the ACES / Blender configs' convention: OCIO's built-in
// CIE-XYZ-D65 → PQ display transforms treat 1.0 as 100 cd/m², and the
// EDR patch lands SDR white at 1.0).
//
// One definition, three implementations that must agree: the C++ below
// (LUT export, tests), kLinearStageMsl (Metal) and kLinearStageHlsl
// (D3D11). The GPU uniform block is all float4 so C++, MSL and HLSL lay
// it out identically.

#pragma once

#include <algorithm>
#include <cmath>

namespace qcv {

// Which interchange role the split chain passes through, from the
// reference-space side of the colourspace entering the Display/View.
enum class InterchangeSide : int {
    None    = 0,   // no split (chain not configured, data colourspace, no role)
    Scene   = 1,   // aces_interchange (ACES2065-1, AP0, D60 white)
    Display = 2,   // cie_xyz_d65_interchange (CIE XYZ, D65 white)
};

// User settings for the stage, as stored on OCIOConfigManager.
struct LinearStageSettings {
    float gain            = 1.0f;     // linear multiplier
    bool  kneeEnabled     = false;
    float kneeSourceNits  = 1000.0f;  // content peak that maps onto the target
    float kneeTargetNits  = 1000.0f;  // used when the display is not SDR
    float kneeStart       = -1.0f;    // fraction of the target in PQ; < 0 = BT.2390
};

// GPU uniform block. Rows are float4 with w unused.
struct LinearStageGpu {
    float to0[4], to1[4], to2[4];        // interchange → linear Rec.2020
    float from0[4], from1[4], from2[4];  // linear Rec.2020 → interchange
    float p0[4];   // x = gain, y = knee on (0/1), z = PQ(source peak), w = target / source (PQ-normalized)
    float p1[4];   // x = knee start (PQ-normalized), yzw unused
};

namespace linear_stage {

// ST 2084. y = luminance / 10000 nits.
inline float pqEncode(float y)
{
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(std::max(y, 0.0f), m1);
    return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

inline float pqDecode(float e)
{
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(std::clamp(e, 0.0f, 1.0f), 1.0f / m2);
    return std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

// BT.2390's default knee start for a PQ-normalized target peak.
inline float bt2390KneeStart(float maxLum)
{
    return std::max(0.0f, 1.5f * maxLum - 0.5f);
}

// Knee start in nits, for UI read-outs.
inline float kneeStartNits(float kneeStartNorm, float sourceNits)
{
    return pqDecode(kneeStartNorm * pqEncode(sourceNits / 10000.0f)) * 10000.0f;
}

inline void setRow(float *row, float a, float b, float c)
{
    row[0] = a; row[1] = b; row[2] = c; row[3] = 0.0f;
}

// True when the stage changes nothing, so the unsplit chain can be used.
inline bool isIdentity(const LinearStageSettings &s)
{
    return std::abs(s.gain - 1.0f) < 1e-6f && !s.kneeEnabled;
}

// Resolve the user settings into the GPU block. `displayIsSdr`: the
// active Display/View targets SDR, so the knee's target is 100 nits.
inline LinearStageGpu resolve(const LinearStageSettings &s,
                              InterchangeSide side,
                              bool displayIsSdr)
{
    LinearStageGpu g{};
    if (side == InterchangeSide::Scene) {
        // AP0 (D60) → Rec.2020 (D65), Bradford adaptation.
        setRow(g.to0,   +1.4904095205f, -0.2661709193f, -0.2242386013f);
        setRow(g.to1,   -0.0801674999f, +1.1821671211f, -0.1019996212f);
        setRow(g.to2,   +0.0032276312f, -0.0347764757f, +1.0315488446f);
        setRow(g.from0, +0.6790856347f, +0.1577009146f, +0.1632134506f);
        setRow(g.from1, +0.0460020031f, +0.8590546730f, +0.0949433239f);
        setRow(g.from2, -0.0005739432f, +0.0284677684f, +0.9721061748f);
    } else if (side == InterchangeSide::Display) {
        // CIE XYZ (D65) → Rec.2020.
        setRow(g.to0,   +1.7166511880f, -0.3556707838f, -0.2533662814f);
        setRow(g.to1,   -0.6666843518f, +1.6164812366f, +0.0157685458f);
        setRow(g.to2,   +0.0176398574f, -0.0427706133f, +0.9421031212f);
        setRow(g.from0, +0.6369580483f, +0.1446169036f, +0.1688809752f);
        setRow(g.from1, +0.2627002120f, +0.6779980715f, +0.0593017165f);
        setRow(g.from2, +0.0000000000f, +0.0280726930f, +1.0609850577f);
    } else {
        setRow(g.to0, 1, 0, 0);   setRow(g.to1, 0, 1, 0);   setRow(g.to2, 0, 0, 1);
        setRow(g.from0, 1, 0, 0); setRow(g.from1, 0, 1, 0); setRow(g.from2, 0, 0, 1);
    }

    const float srcNits = std::max(s.kneeSourceNits, 1.0f);
    const float tgtNits = displayIsSdr ? 100.0f : std::max(s.kneeTargetNits, 1.0f);
    const float pqSrc   = pqEncode(srcNits / 10000.0f);
    const float maxLum  = pqEncode(tgtNits / 10000.0f) / pqSrc;
    // A target at or above the source peak has nothing to compress.
    const bool  knee    = s.kneeEnabled && maxLum < 0.999f;
    float ks = s.kneeStart < 0.0f ? bt2390KneeStart(maxLum)
                                  : s.kneeStart * maxLum;
    ks = std::clamp(ks, 0.0f, maxLum * 0.999f);

    g.p0[0] = s.gain;
    g.p0[1] = knee ? 1.0f : 0.0f;
    g.p0[2] = pqSrc;
    g.p0[3] = maxLum;
    g.p1[0] = ks;
    return g;
}

// CPU reference. `rgb` is in the interchange space, 1.0 = 100 nits.
inline void apply(float *rgb, const LinearStageGpu &g)
{
    float r[3];
    for (int i = 0; i < 3; ++i) {
        const float *row = i == 0 ? g.to0 : (i == 1 ? g.to1 : g.to2);
        r[i] = (row[0] * rgb[0] + row[1] * rgb[1] + row[2] * rgb[2]) * g.p0[0];
    }
    if (g.p0[1] > 0.5f) {
        const float m = std::max(r[0], std::max(r[1], r[2]));
        if (m > 0.0f) {
            const float e1 = std::min(pqEncode(m * 0.01f) / g.p0[2], 1.0f);
            const float ks = g.p1[0], ml = g.p0[3];
            float e2 = e1;
            if (e1 >= ks) {
                const float t = (e1 - ks) / (1.0f - ks);
                const float t2 = t * t, t3 = t2 * t;
                e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks)
                   + (-2 * t3 + 3 * t2) * ml;
            }
            const float m2 = pqDecode(e2 * g.p0[2]) * 100.0f;
            const float k = m2 / m;
            r[0] *= k; r[1] *= k; r[2] *= k;
        }
    }
    for (int i = 0; i < 3; ++i) {
        const float *row = i == 0 ? g.from0 : (i == 1 ? g.from1 : g.from2);
        rgb[i] = row[0] * r[0] + row[1] * r[1] + row[2] * r[2];
    }
}

} // namespace linear_stage

// Metal. Declares `QcvStage` and `qcvLinearStage(float4, constant QcvStage&)`.
constexpr const char *kLinearStageMsl = R"(
struct QcvStage {
    float4 to0, to1, to2;
    float4 from0, from1, from2;
    float4 p0;
    float4 p1;
};

static float qcv_pq_enc(float y)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(max(y, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

static float qcv_pq_dec(float e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(clamp(e, 0.0, 1.0), 1.0 / m2);
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

static float4 qcvLinearStage(float4 c, constant QcvStage &s)
{
    float3 r = float3(dot(s.to0.xyz, c.rgb), dot(s.to1.xyz, c.rgb),
                      dot(s.to2.xyz, c.rgb)) * s.p0.x;
    if (s.p0.y > 0.5) {
        float m = max(r.r, max(r.g, r.b));
        if (m > 0.0) {
            float e1 = min(qcv_pq_enc(m * 0.01) / s.p0.z, 1.0);
            float ks = s.p1.x, ml = s.p0.w;
            float e2 = e1;
            if (e1 >= ks) {
                float t = (e1 - ks) / (1.0 - ks);
                float t2 = t * t, t3 = t2 * t;
                e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks
                   + (t3 - 2.0 * t2 + t) * (1.0 - ks)
                   + (-2.0 * t3 + 3.0 * t2) * ml;
            }
            r *= (qcv_pq_dec(e2 * s.p0.z) * 100.0) / m;
        }
    }
    return float4(dot(s.from0.xyz, r), dot(s.from1.xyz, r),
                  dot(s.from2.xyz, r), c.a);
}
)";

// D3D11 (SM 5.0). Declares cbuffer `QcvStageCb` at b0 and
// `qcvLinearStage(float4)`.
constexpr const char *kLinearStageHlsl = R"(
cbuffer QcvStageCb : register(b0)
{
    float4 qcvTo0, qcvTo1, qcvTo2;
    float4 qcvFrom0, qcvFrom1, qcvFrom2;
    float4 qcvP0;
    float4 qcvP1;
};

float qcv_pq_enc(float y)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(max(y, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float qcv_pq_dec(float e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(saturate(e), 1.0 / m2);
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

float4 qcvLinearStage(float4 c)
{
    float3 r = float3(dot(qcvTo0.xyz, c.rgb), dot(qcvTo1.xyz, c.rgb),
                      dot(qcvTo2.xyz, c.rgb)) * qcvP0.x;
    if (qcvP0.y > 0.5) {
        float m = max(r.r, max(r.g, r.b));
        if (m > 0.0) {
            float e1 = min(qcv_pq_enc(m * 0.01) / qcvP0.z, 1.0);
            float ks = qcvP1.x, ml = qcvP0.w;
            float e2 = e1;
            if (e1 >= ks) {
                float t = (e1 - ks) / (1.0 - ks);
                float t2 = t * t, t3 = t2 * t;
                e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks
                   + (t3 - 2.0 * t2 + t) * (1.0 - ks)
                   + (-2.0 * t3 + 3.0 * t2) * ml;
            }
            r *= (qcv_pq_dec(e2 * qcvP0.z) * 100.0) / m;
        }
    }
    return float4(dot(qcvFrom0.xyz, r), dot(qcvFrom1.xyz, r),
                  dot(qcvFrom2.xyz, r), c.a);
}
)";

} // namespace qcv
