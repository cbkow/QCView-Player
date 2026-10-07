/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 * Derived from OpenDRT v1.1.0 by Jed Smith (https://github.com/jedypod/open-display-transform), GPLv3;
 * modified 2026-09-30 to 2026-10-04, see CHANGES-FROM-OPENDRT.md. Not affiliated with or endorsed by OpenDRT.
 */
/* DRT core — the parameter block every host shares.
 *
 * Port of OpenDRT v1.1.0 by Jed Smith (https://github.com/jedypod/open-display-transform).
 * License: GPL-3.0 (same as upstream). See LICENSE.
 *
 * WHY THIS SHAPE
 *   One flat struct of 4-byte scalars (int / float), total count a multiple of 4.
 *   - C++:   natural layout, sizeof == 4 * count.
 *   - MSL:   `constant DrtParams&` — natural layout, identical.
 *   - HLSL:  cbuffer packing puts scalars back to back in float4 registers and only
 *            pads when a member would straddle a register; every member here is one
 *            scalar, so nothing straddles and nothing pads.
 *   - GLSL:  std140 aligns scalars to 4 bytes and rounds the struct to 16; the count
 *            is a multiple of 4, so the rounding is a no-op.
 *   So the same bytes the C++ host fills can be uploaded as-is to every GPU dialect.
 *   Keep it that way: no vec3, no arrays, no bool, no double, and pad any addition
 *   back to a multiple of 4 (the reserved_* slots exist for that).
 *
 *   Enumerations are #defines, not enums, because this header is included by
 *   shader dialects that have no enum.
 *
 * TWO HALVES
 *   "User" fields are the StickShift parameter set of the upstream DCTL, plus the
 *   four selectors the DCTL reads directly (input gamut / transfer, display gamut /
 *   EOTF). The AE effect, the another host panel and the presets file all speak in these.
 *   "Derived" fields are the per-render constants the upstream DCTL recomputes on
 *   every pixel because DCTL has no other place to put them ("These should be
 *   pre-calculated but there is no way to do this in DCTL"). drt_derive() fills them
 *   once per parameter change on the host; drt_transform() only reads them.
 */
#ifndef OPENDRT_PARAMS_H
#define OPENDRT_PARAMS_H

/* in_gamut — order is the upstream DCTL combo box order; matrices are in the kernel. */
#define DRT_IN_XYZ            0
#define DRT_IN_AP0            1   /* ACES 2065-1, CAT02 to D65 inside the matrix */
#define DRT_IN_AP1            2   /* ACEScg,      CAT02 to D65 inside the matrix */
#define DRT_IN_P3D65          3
#define DRT_IN_REC2020        4
#define DRT_IN_REC709         5
#define DRT_IN_ARRI_WG3       6
#define DRT_IN_ARRI_WG4       7
#define DRT_IN_RED_WG         8
#define DRT_IN_SONY_SGAMUT3   9
#define DRT_IN_SONY_SGAMUT3CINE 10
#define DRT_IN_PANASONIC_VGAMUT 11
#define DRT_IN_FILMLIGHT_EGAMUT 12
#define DRT_IN_FILMLIGHT_EGAMUT2 13
#define DRT_IN_DAVINCI_WG     14
#define DRT_IN_GAMUT_COUNT    15

/* in_oetf — upstream order. */
#define DRT_OETF_LINEAR       0
#define DRT_OETF_DAVINCI_INTERMEDIATE 1
#define DRT_OETF_FILMLIGHT_TLOG 2
#define DRT_OETF_ACESCCT      3
#define DRT_OETF_ARRI_LOGC3   4
#define DRT_OETF_ARRI_LOGC4   5
#define DRT_OETF_RED_LOG3G10  6
#define DRT_OETF_PANASONIC_VLOG 7
#define DRT_OETF_SONY_SLOG3   8
#define DRT_OETF_FUJI_FLOG2   9
/* Display-referred decodes, NOT in upstream (appended so the DCTL indices stay).
   Needed wherever the source is a delivery rather than a camera file: an AE comp
   is mostly Rec.709 video. Decoding an EOTF as if it were an inverse picture
   formation is the usual approximation (a Nuke Read set to rec709). */
#define DRT_OETF_REC1886      10  /* 2.4 power */
#define DRT_OETF_SRGB         11  /* IEC 61966-2-1 piecewise */
#define DRT_OETF_POWER_2_2    12
#define DRT_OETF_BT709_CAMERA 13  /* inverse of the BT.709 camera OETF (0.45 power / 4.5 linear) */
#define DRT_OETF_PQ_100       14  /* ST 2084, scaled so 100 nits = 1.0 (OpenDRT's own PQ convention) */
#define DRT_OETF_HLG_1000     15  /* BT.2100 HLG at 1000 nits, 100 nits = 1.0 */
/* OpenDRT inverse entries (drt_inverse_transform): the file is a delivery made
   by this look through the named display encoding. Each entry names an encoding
   so the transfer popup fully describes the file; the host maps the entry onto
   display_gamut / eotf before deriving (drt_inverse_display()); tn_su (how the
   master was viewed) stays a parameter. in_gamut is ignored for these. Anything >= DRT_OETF_INVERSE_FIRST is an inverse. */
#define DRT_OETF_INVERSE_FIRST    16
#define DRT_OETF_INVERSE_REC1886  16  /* 2.4 power / Rec.709 */
#define DRT_OETF_INVERSE_SRGB     17  /* 2.2 power / Rec.709 */
#define DRT_OETF_INVERSE_P3       18  /* 2.2 power / P3-D65 */
#define DRT_OETF_INVERSE_PQ       19  /* ST 2084 / Rec.2020 (P3 limited) */
#define DRT_OETF_INVERSE_HLG      20  /* HLG / Rec.2020 (P3 limited) */
#define DRT_OETF_INVERSE_PQ_P3    21  /* ST 2084 / P3-D65 container (the Dolby display preset) */
#define DRT_OETF_COUNT            22
#define DRT_OETF_UPSTREAM_COUNT 10 /* the ones the reference DCTL knows; the probe compares only these */

/* display_gamut — upstream order. */
#define DRT_DG_REC709         0
#define DRT_DG_P3D65          1
#define DRT_DG_REC2020_P3LIM  2   /* Rec.2020 container, P3 limited */
#define DRT_DG_P3D60          3
#define DRT_DG_P3DCI          4
#define DRT_DG_XYZ            5   /* DCDM X'Y'Z' */
#define DRT_DG_WORKING        6   /* not in upstream: the working gamut itself (p.working_gamut), linear hand-off */
#define DRT_DG_AP0            7   /* not in upstream: ACES 2065-1, linear hand-off (scene-referred delivery) */
#define DRT_DG_AP1            8   /* not in upstream: ACEScg, linear hand-off */
#define DRT_DG_REC2020        9   /* not in upstream: Rec.2020, the plain container (no P3 limit), linear hand-off */
#define DRT_DG_COUNT          10

/* eotf — upstream order. 1..3 are pure powers 2.2 / 2.4 / 2.6 (2.0 + 0.2 * eotf). */
#define DRT_EOTF_LINEAR       0
#define DRT_EOTF_POWER_2_2    1
#define DRT_EOTF_POWER_2_4    2
#define DRT_EOTF_POWER_2_6    3
#define DRT_EOTF_PQ           4
#define DRT_EOTF_HLG          5
#define DRT_EOTF_COUNT        6

/* tn_su — surround. */
#define DRT_SURROUND_DARK     0
#define DRT_SURROUND_DIM      1
#define DRT_SURROUND_BRIGHT   2

/* cwp — creative white. Upstream's StickShift order (D93 first). */
#define DRT_CWP_D93           0
#define DRT_CWP_D75           1
#define DRT_CWP_D65           2
#define DRT_CWP_D60           3
#define DRT_CWP_D55           4
#define DRT_CWP_D50           5
#define DRT_CWP_COUNT         6

struct DrtParams {
  /* ---------------- selectors and switches (int) : 20 ---------------- */
  int in_gamut;          /* DRT_IN_*        */
  int in_oetf;           /* DRT_OETF_*      */
  int display_gamut;     /* DRT_DG_*        */
  int eotf;              /* DRT_EOTF_*      */
  int tn_su;             /* DRT_SURROUND_*  */
  int cwp;               /* DRT_CWP_*       */
  int clamp_out;         /* clamp final RGB to 0..1 (upstream "clamp"; renamed: clamp() is a builtin in every shader dialect) */
  int tn_hcon_enable;
  int tn_lcon_enable;
  int pt_enable;         /* exposed by the upstream UI but NOT read by the v1.1.0 kernel; kept so presets round-trip */
  int ptl_enable;
  int ptm_enable;
  int brl_enable;
  int brlp_enable;
  int hc_enable;
  int hs_rgb_enable;
  int hs_cmy_enable;
  int working_gamut;     /* DRT_IN_* index of the linear working gamut drt_input_transform() lands in (Input effect only) */
  int in_range;          /* 0 = full range codes, 1 = limited (video) range: the host's decode handed
                            over 64..940 of 1023 unexpanded; expanded before any transfer (Input only) */
  int out_view;          /* Output: 0 = OpenDRT rendering, 1 = Un-tone-mapped (the input conversion
                            reversed: gamut matrix, display curve, nothing else; OCIO's view of that name) */
  /* Output, Mode: 0 = View (the rows above and the Display group), 1 = Render: the rnd_* block
     replaces display_gamut / eotf / tn_Lp / tn_su / out_view before derive (drt_output_mode()).
     The look rows are shared: what was approved on the monitor is what renders; only the
     encoding differs per destination. */
  int out_mode;
  int rnd_gamut;         /* DRT_DG_*   */
  int rnd_eotf;          /* DRT_EOTF_* */
  int rnd_su;            /* DRT_SURROUND_* */
  int rnd_view;          /* 0 OpenDRT, 1 Un-tone-mapped */
  int reserved_i3;
  int kn_auto;           /* Knee (core/mincolor_knee.h): 1 = BT.2390 knee start */
  int reserved_i5;

  /* ---------------- user floats : 60 ---------------- */
  /* tonescale */
  float tn_Lp;           /* display peak luminance, nits (100..1000 in the UI) */
  float tn_gb;           /* HDR grey boost */
  float pt_hdr;          /* HDR purity */
  float tn_Lg;           /* display grey luminance, nits */
  float tn_con;          /* contrast */
  float tn_sh;           /* shoulder clip */
  float tn_toe;
  float tn_off;
  float tn_hcon;
  float tn_hcon_pv;
  float tn_hcon_st;
  float tn_lcon;
  float tn_lcon_w;
  float cwp_lm;          /* creative white limit */
  /* render space */
  float rs_sa;
  float rs_rw;
  float rs_bw;
  /* purity compress high */
  float pt_lml;
  float pt_lml_r;
  float pt_lml_g;
  float pt_lml_b;
  float pt_lmh;
  float pt_lmh_r;
  float pt_lmh_b;
  /* purity softclip */
  float ptl_c;
  float ptl_m;
  float ptl_y;
  /* mid purity */
  float ptm_low;
  float ptm_low_rng;
  float ptm_low_st;
  float ptm_high;
  float ptm_high_rng;
  float ptm_high_st;
  /* brilliance */
  float brl;
  float brl_r;
  float brl_g;
  float brl_b;
  float brl_rng;
  float brl_st;
  /* post brilliance */
  float brlp;
  float brlp_r;
  float brlp_g;
  float brlp_b;
  /* hue contrast */
  float hc_r;
  float hc_r_rng;
  /* hue shift rgb */
  float hs_r;
  float hs_r_rng;
  float hs_g;
  float hs_g_rng;
  float hs_b;
  float hs_b_rng;
  /* hue shift cmy */
  float hs_c;
  float hs_c_rng;
  float hs_m;
  float hs_m_rng;
  float hs_y;
  float hs_y_rng;
  float inv_cap;         /* inverse only: display values are held under this fraction of the peak
                            luminance before inverting (1 = off). The tonescale's ceiling makes the
                            inverse unbounded at a master's white: a PQ master's rim pixels invert to
                            negative, hundreds-high scene values that any grade turns into colour. */
  float kn_src;          /* Knee: source peak, nits */
  float rnd_Lp;          /* Output, Render block: peak luminance, nits */

  /* ---------------- derived by drt_derive() : 36 ---------------- */
  /* input gamut -> XYZ D65, row major (the kernel applies xyz -> P3-D65 after it,
     as two multiplies like upstream, so the float rounding matches the DCTL exactly) */
  float in_m00; float in_m01; float in_m02;
  float in_m10; float in_m11; float in_m12;
  float in_m20; float in_m21; float in_m22;
  /* tonescale constraint constants (names follow the upstream DCTL) */
  float ts_x0;
  float ts_p;
  float ts_s;
  float ts_s1;
  float s_Lp100;
  float ts_m2;
  float ts_dsc;
  /* contrast low / high */
  float lcon_m;
  float lcon_w;
  float lcon_cnst_sc;
  float hcon_p;
  /* render space weights */
  float rs_wr;
  float rs_wg;
  float rs_wb;
  float kn_pq_src;       /* Knee, derived by drt_knee_derive(): PQ(source peak) */
  /* XYZ D65 -> working gamut, row major: the inverse of working_gamut's forward
     matrix, for drt_input_transform() (Input effect). Identity when unused. */
  float wk_m00; float wk_m01; float wk_m02;
  float wk_m10; float wk_m11; float wk_m12;
  float wk_m20; float wk_m21; float wk_m22;
  /* Knee: two user fields and one derived, in the derived block's spare slots so the
     struct keeps its size (drt_derive() leaves them alone) */
  float kn_tgt;          /* Knee: target peak, nits */
  float kn_start;        /* Knee: knee start, fraction of the target in PQ (kn_auto 0) */
  float kn_max_lum;      /* Knee, derived: PQ(target) / PQ(source) */
};

#define DRT_PARAMS_SCALARS 124   /* 28 + 60 + 36; sizeof(DrtParams) == 496 on every dialect */

#endif /* OPENDRT_PARAMS_H */
