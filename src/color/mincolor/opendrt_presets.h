/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 * Derived from OpenDRT v1.1.0 by Jed Smith (https://github.com/jedypod/open-display-transform), GPLv3;
 * modified 2026-09-30 to 2026-10-04, see CHANGES-FROM-OPENDRT.md. Not affiliated with or endorsed by OpenDRT.
 */
/* DRT core — presets and the preset-mode resolver (C++ only, host side).
 *
 * The upstream DCTL has two modes: preset mode (look / tonescale / creative white /
 * display combo boxes, the shipped default) and StickShift (every parameter exposed,
 * enabled by editing the file). This header carries the preset tables transcribed
 * from the DCTL and a resolver that reproduces the DCTL's preset-mode logic exactly,
 * so a host can offer both modes over one DrtParams.
 *
 * The field lists are X-macros so the struct, the apply function and the JSON
 * dumper (tools/dump_presets.cpp) cannot drift apart. Included by opendrt.h; the
 * tables live in opendrt_presets.cpp.
 */
#pragma once

namespace drt {

/* Look preset fields, in the exact order the DCTL preset lines assign them. */
#define DRT_LOOK_FIELDS(F, I) \
    F(tn_con) F(tn_sh) F(tn_toe) F(tn_off) \
    I(tn_hcon_enable) F(tn_hcon) F(tn_hcon_pv) F(tn_hcon_st) \
    I(tn_lcon_enable) F(tn_lcon) F(tn_lcon_w) \
    I(cwp) F(cwp_lm) \
    F(rs_sa) F(rs_rw) F(rs_bw) \
    I(pt_enable) F(pt_lml) F(pt_lml_r) F(pt_lml_g) F(pt_lml_b) F(pt_lmh) F(pt_lmh_r) F(pt_lmh_b) \
    I(ptl_enable) F(ptl_c) F(ptl_m) F(ptl_y) \
    I(ptm_enable) F(ptm_low) F(ptm_low_rng) F(ptm_low_st) F(ptm_high) F(ptm_high_rng) F(ptm_high_st) \
    I(brl_enable) F(brl) F(brl_r) F(brl_g) F(brl_b) F(brl_rng) F(brl_st) \
    I(brlp_enable) F(brlp) F(brlp_r) F(brlp_g) F(brlp_b) \
    I(hc_enable) F(hc_r) F(hc_r_rng) \
    I(hs_rgb_enable) F(hs_r) F(hs_r_rng) F(hs_g) F(hs_g_rng) F(hs_b) F(hs_b_rng) \
    I(hs_cmy_enable) F(hs_c) F(hs_c_rng) F(hs_m) F(hs_m_rng) F(hs_y) F(hs_y_rng)

/* Tonescale preset fields: the tonescale subset of the above. */
#define DRT_TONESCALE_FIELDS(F, I) \
    F(tn_con) F(tn_sh) F(tn_toe) F(tn_off) \
    I(tn_hcon_enable) F(tn_hcon) F(tn_hcon_pv) F(tn_hcon_st) \
    I(tn_lcon_enable) F(tn_lcon) F(tn_lcon_w)

struct DrtLook {
    const char *name;
#define DRT_X_F(n) float n;
#define DRT_X_I(n) int n;
    DRT_LOOK_FIELDS(DRT_X_F, DRT_X_I)
#undef DRT_X_F
#undef DRT_X_I
};

struct DrtTonescale {
    const char *name;
#define DRT_X_F(n) float n;
#define DRT_X_I(n) int n;
    DRT_TONESCALE_FIELDS(DRT_X_F, DRT_X_I)
#undef DRT_X_F
#undef DRT_X_I
};

/* Display encoding preset. The DCTL sets surround, gamut and EOTF; peak luminance
   stays on the user's slider. default_Lp is what the Nuke node's display presets
   set (100 for SDR, 1000 for PQ/HLG) so hosts can offer the same convenience. */
struct DrtDisplay {
    const char *name;
    int   eotf;
    int   display_gamut;
    int   tn_su;
    float default_Lp;
};

/* Tables (opendrt_presets.cpp). Indices match the DCTL combo boxes. */
constexpr int kLookCount      = 8;   /* Standard, Arriba, Sylvan, Colorful, Aery, Dystopic, Umbra, Base */
constexpr int kTonescaleCount = 13;  /* Low Contrast ... DaGrinchi ToneGroan (DCTL index 1..13) */
constexpr int kDisplayUpstreamCount = 9;   /* the DCTL's presets; the probe compares these */
constexpr int kDisplayCount   = 14;        /* + None (linear): working gamut, ACES 2065-1, ACEScg, Rec.2020, Rec.709 */
constexpr int kCwpCount       = 6;

extern const DrtLook      kLooks[kLookCount];
extern const DrtTonescale kTonescales[kTonescaleCount];
extern const DrtDisplay   kDisplays[kDisplayCount];
extern const char *const  kCwpNames[kCwpCount];       /* D93 D75 D65 D60 D55 D50 */
extern const char *const  kInGamutNames[DRT_IN_GAMUT_COUNT];
extern const char *const  kInOetfNames[DRT_OETF_COUNT];

/* The DCTL's own defaults: DEFINE_UI_PARAMS values of the StickShift block, plus
   the shipped selector defaults (DaVinci Wide Gamut / DaVinci Intermediate in,
   Rec.1886 out, dim surround, clamp on). Equal to the Standard look. Not derived. */
DrtParams drt_stickshift_defaults();

void drt_apply_look(DrtParams &p, const DrtLook &l);
void drt_apply_tonescale(DrtParams &p, const DrtTonescale &t);
void drt_apply_look_tonescale(DrtParams &p, const DrtLook &l);   /* the tonescale rows of a look only ("Use Look") */
void drt_output_mode(DrtParams &p);
int  drt_display_preset_index(int display_gamut, int eotf);   /* kDisplays index of that pair, or -1 */   /* Output: in Render mode, the rnd_* block replaces the view's encoding; call before drt_derive() */
void drt_apply_display(DrtParams &p, const DrtDisplay &d);   /* eotf, gamut, surround only */

/* For an inverse in_oetf entry, set display_gamut / eotf to the encoding
   that entry names (peak stays on tn_Lp, surround on tn_su). Returns false and changes nothing for
   any other transfer. Hosts call it before drt_derive() on the Input side. */
bool drt_inverse_display(DrtParams &p);
extern const int kInverseDisplayMap[];

/* Preset mode, as the DCTL's UI exposes it. Defaults here are the host-side choice
   for a colour-managed compositor (linear ACEScg in), not the DCTL's Resolve
   defaults; see drt_stickshift_defaults() for those. */
struct DrtSettings {
    int   in_gamut  = DRT_IN_AP1;
    int   in_oetf   = DRT_OETF_LINEAR;
    float tn_Lp     = 100.0f;
    float tn_gb     = 0.13f;
    float pt_hdr    = 0.5f;
    float tn_Lg     = 10.0f;
    int   look      = 0;      /* kLooks index */
    int   tonescale = 0;      /* 0 = use the look's tonescale, 1..13 = kTonescales[n-1] */
    int   cwp       = 0;      /* 0 = use the look's creative white, 1..6 = kCwpNames[n-1] */
    float cwp_lm    = 0.25f;  /* only applied when cwp != 0, as in the DCTL */
    int   display   = 0;      /* kDisplays index */
};

/* Reproduces the DCTL preset-mode block exactly (look, then tonescale override,
   clamp forced on, creative white override, display preset), then drt_derive(). */
DrtParams drt_resolve(const DrtSettings &s);

/* Grade defaults: every corrector at identity, Resolve-like zone boundaries for a
   100-nit display, a 60-degree red secondary window switched off. Not derived. */
DrtGradeParams drt_grade_defaults();

} // namespace drt
