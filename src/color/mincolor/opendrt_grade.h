/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow. Part of a work derived from OpenDRT v1.1.0 by Jed Smith (GPLv3); see NOTICE.
 */
/* minColor Grade — a colour corrector whose luminance zones are defined through
 * the OpenDRT tonescale. Not part of upstream OpenDRT.
 *
 * Sits between Input and Output in the linear working gamut. Order: everything
 * that SELECTS pixels is measured on the source pixel (the six zone weights and
 * the secondary's key), then everything that CHANGES them, global before local:
 *   temperature / tint -> global exposure -> contrast about a pivot -> six zones
 *   (Black, Dark, Shadow, Light, Highlight, Specular) -> global saturation ->
 *   HSL secondary.
 * So no dial can move another dial's target: Exposure does not shift a pixel into
 * the next zone, a zone's tint does not push a colour out of the secondary's hue
 * window, and Contrast does not re-shape a zone's push (2026-10-03; until then the
 * zones were measured after Exposure and the key after the zones).
 * A zone is a range of SCENE STOPS over mid grey: each pixel's tonescale norm (the
 * quantity the DRT tonescales) is measured in stops relative to the norm of 0.18
 * grey, and six windows over that axis are the zone weights. The contrast pivot
 * and the secondary's level window are on the same axis (stops over grey), so the
 * Grade's maths does not depend on the DRT reference's peak or look at all; the
 * reference serves only the hosts' eyedroppers. A zone has full
 * weight across its band and feathers smoothly to zero over `zfalloff` stops
 * beyond each boundary, so neighbours overlap and the weights do NOT sum to one
 * (Lumetri's bands behave the same way; a push on two adjacent zones adds up in
 * the feather between them). Boundaries default to -3, -1.5, 0, +1.5, +3 stops
 * with a 2-stop feather. The top zones split real highlight range instead of
 * the last few nits of an SDR shoulder. (The zones were display nits with
 * telescoping sum-to-one windows until 0.3; the contrast pivot and the
 * secondary's luminance window are still in nits.)
 *
 * Same dialect rules as opendrt_kernel.h, which must be included first (with
 * opendrt_params.h). DrtGradeParams is a flat 4-byte block like DrtParams, so the
 * host uploads it as a second uniform buffer. The DrtParams `p` is the DRT
 * reference: the host sets p.in_gamut to the working gamut (so in_m** is
 * working -> XYZ) and the look / display rows to match the Output on top.
 */
#ifndef OPENDRT_GRADE_H
#define OPENDRT_GRADE_H

#define DRT_ZONE_COUNT 6   /* Black, Dark, Shadow, Light, Highlight, Specular */

struct DrtGradeParams {
  /* ---- switches : 4 ---- */
  int sec_enable;
  int sec_show_mask;
  int reserved_i0;
  int reserved_i1;

  /* ---- global : 6 ---- */
  float exposure;        /* stops */
  float contrast;        /* 1 = none; power about the pivot, applied on the norm */
  float pivot_nits;      /* RETIRED (hidden row keeps its id): display nits the contrast pivoted on */
  float saturation;      /* 1 = none */
  float temperature;     /* -1 .. 1, warm positive */
  float tint;            /* -1 .. 1, magenta positive */

  /* ---- zones, 4 per zone, Black first : 24 ---- */
  float z0_exposure; float z0_saturation; float z0_hue; float z0_tint;
  float z1_exposure; float z1_saturation; float z1_hue; float z1_tint;
  float z2_exposure; float z2_saturation; float z2_hue; float z2_tint;
  float z3_exposure; float z3_saturation; float z3_hue; float z3_tint;
  float z4_exposure; float z4_saturation; float z4_hue; float z4_tint;
  float z5_exposure; float z5_saturation; float z5_hue; float z5_tint;

  /* ---- zone ranges : 6 ---- */
  float zb0; float zb1; float zb2; float zb3; float zb4;   /* boundaries, scene stops over grey */
  float zfalloff;                                          /* stops, feather beyond each boundary */

  /* ---- HSL secondary : 11 + 1 pad ---- */
  float sec_hue;         /* degrees, red = 0, the DRT's hue scale */
  float sec_hue_width;   /* degrees, full width */
  float sec_sat_min;     /* purity, 0..2 */
  float sec_sat_max;
  float sec_lum_min;     /* RETIRED (hidden rows keep their ids): the window in nits */
  float sec_lum_max;
  float sec_softness;    /* 0..1 */
  float sec_exposure;
  float sec_saturation;
  float sec_hue_shift;   /* tint hue, degrees */
  float sec_tint;        /* tint amount */
  float pivot_stops;     /* contrast pivot, scene stops over grey (replaces pivot_nits, 2026-10-03) */
  float sec_lev_min;     /* secondary level window, stops over grey (replaces sec_lum_min / max) */
  float sec_lev_max;
  float reserved_f1;
  float reserved_f2;

  /* ---- derived by drt_grade_derive() : 12 ---- */
  float d_pivot_scene;   /* pivot as a scene norm value */
  float d_lb0; float d_lb1; float d_lb2; float d_lb3; float d_lb4;   /* boundaries, log2 of the norm */
  float d_gain_r; float d_gain_g; float d_gain_b;                   /* temperature / tint gains */
  float d_lum_min_l; float d_lum_max_l;                             /* secondary luminance bounds, log2 nits */
  float d_grey_l;        /* log2 of the norm of 0.18 grey: the zones' zero */
};

#define DRT_GRADE_SCALARS 68   /* 4 + 6 + 24 + 6 + 16 + 12; sizeof == 272 on every dialect */

/* ------------------------------------------- neutral tonescale, forward */
/* Scene norm -> display-linear fraction, exactly the achromatic path of
   drt_render_linear(): the twin of drt_tonescale_neutral_inv(). */
__DEVICE__ float drt_tonescale_neutral_fwd(DRT_PARAMS_ARG p, float t) {
  if (p.tn_lcon_enable != 0) {
    t *= p.lcon_cnst_sc;
    t = drt_compress_toe_cubic(t, p.lcon_m, p.lcon_w, 0);
  }
  if (p.tn_hcon_enable != 0) t = drt_contrast_high(t, p.hcon_p, p.tn_hcon_pv, p.tn_hcon_st, 0);
  t = drt_compress_hyperbolic_power(t, p.ts_s, p.ts_p);
  t *= p.ts_m2;
  t = drt_compress_toe_quadratic(t, p.tn_toe, 0);
  t *= p.ts_dsc;
  return t;
}

/* Display-linear fraction (the forward's own convention) -> nits. */
__DEVICE__ float drt_display_nits(DRT_PARAMS_ARG p, float t) {
  if (p.eotf == 4) return t*10000.0f;   /* PQ: ts_dsc put 100 nits at 0.01 */
  if (p.eotf == 5) return t*1000.0f;    /* HLG: 1000-nit system, ts_dsc 0.1 */
  return t*p.tn_Lp;
}
__DEVICE__ float drt_nits_to_display(DRT_PARAMS_ARG p, float nits) {
  if (p.eotf == 4) return nits/10000.0f;
  if (p.eotf == 5) return nits/1000.0f;
  return nits/p.tn_Lp;
}

/* Where the DRT would put this working-gamut pixel: its tonescale norm (the same
   quantity drt_render_linear() computes before the ratios are taken). */
__DEVICE__ float drt_grade_norm(DRT_PARAMS_ARG p, float3 rgb) {
  drt_mat3 wk_to_xyz = drt_make_mat3(
    make_float3(p.in_m00, p.in_m01, p.in_m02),
    make_float3(p.in_m10, p.in_m11, p.in_m12),
    make_float3(p.in_m20, p.in_m21, p.in_m22));
  float3 p3 = drt_vdot(drt_matrix_xyz_to_p3d65, drt_vdot(wk_to_xyz, rgb));
  float sat_L = p3.x*p.rs_wr + p3.y*p.rs_wg + p3.z*p.rs_wb;
  p3 = sat_L*p.rs_sa + p3*(1.0f - p.rs_sa);
  p3 += p.tn_off;
  return drt_hypotf3(p3)/DRT_SQRT3;
}

__DEVICE__ float drt_log2_nits(DRT_PARAMS_ARG p, float norm) {
  float n = drt_display_nits(p, drt_tonescale_neutral_fwd(p, _fmaxf(norm, 0.0f)));
  return _log2f(_fmaxf(n, 1e-4f));
}

/* Smooth step over [-0.5, 0.5] in units of the falloff (the secondary's luminance window). */
__DEVICE__ float drt_zone_step(float x) {
  float u = drt_clampf(x + 0.5f, 0.0f, 1.0f);
  return u*u*(3.0f - 2.0f*u);
}

/* Zone feather: 1 at x <= 0 (inside the band), smooth to 0 at x >= 1 (one falloff past the boundary). */
__DEVICE__ float drt_zone_edge(float x) {
  float u = drt_clampf(x, 0.0f, 1.0f);
  return 1.0f - u*u*(3.0f - 2.0f*u);
}

/* Zero-sum RGB direction for a hue on the DRT's hue scale (red = 0). Inverse of
   drt_opponent(): opp = (R - B, G - (R + B)/2), hue = atan2(opp.x, opp.y) + PI + 1.107. */
/* sin on [-PI, PI] by Taylor to the 7th power: error < 2e-3 at the ends, far
   better than a tint direction needs. sin and cos are not in the shim's intrinsic
   list (the DCTL never needed them), so this keeps the dialect contract intact. */
__DEVICE__ float drt_sin_pi(float x) {
  float x2 = x*x;
  return x*(1.0f - x2*(1.0f/6.0f) + x2*x2*(1.0f/120.0f) - x2*x2*x2*(1.0f/5040.0f));
}

__DEVICE__ float3 drt_hue_dir(float hue_deg) {
  float a = hue_deg*(DRT_PI/180.0f) - DRT_PI - 1.10714931f;
  /* reduce to [-PI, PI] */
  a = _fmod(a + DRT_PI, 2.0f*DRT_PI);
  if (a < 0.0f) a += 2.0f*DRT_PI;
  a -= DRT_PI;
  float ox = drt_sin_pi(a);
  float oy = drt_sin_pi(DRT_PI*0.5f - (a < 0.0f ? -a : a));   /* cos(a) = sin(PI/2 - |a|) */
  float g = (2.0f/3.0f)*oy;
  float r = (ox - (2.0f/3.0f)*oy)*0.5f;
  float b = (-ox - (2.0f/3.0f)*oy)*0.5f;
  float3 d = make_float3(r, g, b);
  float len = drt_hypotf3(d);
  return len > 1e-6f ? d/len : make_float3(0.0f, 0.0f, 0.0f);
}

/* ----------------------------------------------------------------- derive */
__DEVICE__ DrtGradeParams drt_grade_derive(DrtGradeParams g, DRT_PARAMS_ARG p) {
  g.d_grey_l = _log2f(_fmaxf(drt_grade_norm(p, make_float3(0.18f, 0.18f, 0.18f)), 1e-6f));
  g.d_pivot_scene = _exp2f(g.d_grey_l + g.pivot_stops);
  g.d_lb0 = g.d_grey_l + g.zb0;
  g.d_lb1 = g.d_grey_l + g.zb1;
  g.d_lb2 = g.d_grey_l + g.zb2;
  g.d_lb3 = g.d_grey_l + g.zb3;
  g.d_lb4 = g.d_grey_l + g.zb4;
  /* temperature / tint as channel gains (von Kries-ish, Lumetri-grade approximation):
     warm = more red, less blue; magenta = less green */
  g.d_gain_r = _powf(2.0f, 0.5f*g.temperature);
  g.d_gain_b = _powf(2.0f, -0.5f*g.temperature);
  g.d_gain_g = _powf(2.0f, -0.5f*g.tint);
  g.d_lum_min_l = g.d_grey_l + g.sec_lev_min;   /* log2 of the norm */
  g.d_lum_max_l = g.d_grey_l + g.sec_lev_max;
  return g;
}

/* --------------------------------------------------------------- helpers */
__DEVICE__ float drt_grade_luma(DRT_PARAMS_ARG p, float3 rgb) {
  return p.in_m10*rgb.x + p.in_m11*rgb.y + p.in_m12*rgb.z;   /* Y of working -> XYZ */
}

/* Apply exposure / saturation / tint with a weight. */
__DEVICE__ float3 drt_grade_apply(DRT_PARAMS_ARG p, float3 rgb, float w, float exposure, float saturation, float hue_deg, float tint) {
  if (w <= 0.0f) return rgb;
  rgb = rgb*_powf(2.0f, w*exposure);
  float y = drt_grade_luma(p, rgb);
  float s = 1.0f + w*(saturation - 1.0f);
  rgb = make_float3(y + (rgb.x - y)*s, y + (rgb.y - y)*s, y + (rgb.z - y)*s);
  if (tint != 0.0f) rgb += drt_hue_dir(hue_deg)*(_fmaxf(y, 0.0f)*0.25f*w*tint);
  return rgb;
}

/* -------------------------------------------------- secondary measure */
/* What the HSL secondary qualifies on: the DRT's opponent hue (degrees, red = 0,
   the same scale as the tint hues), its purity, and the log2 of its tonescale
   norm (stops over grey once d_grey_l is taken off). The hosts' eyedroppers use
   this too, so a pick lands exactly where the mask looks. */
struct drt_sec_measure {
  float hue_deg;
  float purity;
  float log2_norm;
};

__DEVICE__ drt_sec_measure drt_secondary_measure(DRT_PARAMS_ARG p, float3 rgb) {
  float n = drt_grade_norm(p, rgb);
  drt_mat3 wk_to_xyz = drt_make_mat3(
    make_float3(p.in_m00, p.in_m01, p.in_m02),
    make_float3(p.in_m10, p.in_m11, p.in_m12),
    make_float3(p.in_m20, p.in_m21, p.in_m22));
  float3 p3 = drt_vdot(drt_matrix_xyz_to_p3d65, drt_vdot(wk_to_xyz, rgb));
  float3 ratios = drt_sdivf3f(p3 + p.tn_off, _fmaxf(n, 1e-8f));
  float2 opp = drt_opponent(ratios);
  drt_sec_measure m;
  m.purity = drt_hypotf2(opp)/2.0f;
  m.hue_deg = _fmod(_atan2f(opp.x, opp.y) + DRT_PI + 1.10714931f, 2.0f*DRT_PI)*(180.0f/DRT_PI);
  m.log2_norm = _log2f(_fmaxf(n, 1e-6f));
  return m;
}

/* ------------------------------------------------------------------ grade */
__DEVICE__ float3 drt_grade(DrtGradeParams g, DRT_PARAMS_ARG p, float3 rgb) {
  /* 1. select on the source: the zone weights, and the secondary's key */
  float L = _log2f(_fmaxf(drt_grade_norm(p, rgb), 1e-6f));
  float f = _fmaxf(g.zfalloff, 1e-3f);
  /* zone weights: full across the band, feathered `f` stops past each boundary, free to overlap */
  float w0 = drt_zone_edge((L - g.d_lb0)/f);
  float w1 = drt_zone_edge((g.d_lb0 - L)/f)*drt_zone_edge((L - g.d_lb1)/f);
  float w2 = drt_zone_edge((g.d_lb1 - L)/f)*drt_zone_edge((L - g.d_lb2)/f);
  float w3 = drt_zone_edge((g.d_lb2 - L)/f)*drt_zone_edge((L - g.d_lb3)/f);
  float w4 = drt_zone_edge((g.d_lb3 - L)/f)*drt_zone_edge((L - g.d_lb4)/f);
  float w5 = drt_zone_edge((g.d_lb4 - L)/f);
  float mask = 0.0f;
  if (g.sec_enable != 0) {
    /* HSL secondary: qualifier in the DRT's own hue / purity terms, level in stops over grey */
    drt_sec_measure sm = drt_secondary_measure(p, rgb);
    float purity = sm.purity;
    float hue = sm.hue_deg*(DRT_PI/180.0f);
    float Ls = sm.log2_norm;
    float soft = _fmaxf(g.sec_softness, 0.01f);
    /* hue window: signed distance to the centre, soft edge = soft * 30 degrees */
    float hc = g.sec_hue*(DRT_PI/180.0f);
    float dh = _fabs(drt_hue_offset(hue, hc));
    float hw = 0.5f*g.sec_hue_width*(DRT_PI/180.0f);
    float he = soft*(30.0f*DRT_PI/180.0f);
    float mh = 1.0f - drt_zone_step((dh - hw)/he);
    /* purity window, soft edge = soft * 0.2 */
    float pe = soft*0.2f;
    /* Saturation Min 0 means no lower bound, so greys sit fully inside */
    float mp_lo = g.sec_sat_min <= 0.0f ? 1.0f : drt_zone_step((purity - g.sec_sat_min)/pe);
    float mp = mp_lo*(1.0f - drt_zone_step((purity - g.sec_sat_max)/pe));
    /* level window in stops, soft edge = soft * 1 stop */
    float le = soft*1.0f;
    float ml = drt_zone_step((Ls - g.d_lum_min_l)/le)*(1.0f - drt_zone_step((Ls - g.d_lum_max_l)/le));
    mask = mh*mp*ml;
    if (g.sec_show_mask != 0) return make_float3(mask, mask, mask);
  }

  /* 2. global: temperature / tint, exposure */
  rgb = make_float3(rgb.x*g.d_gain_r, rgb.y*g.d_gain_g, rgb.z*g.d_gain_b);
  rgb = rgb*_powf(2.0f, g.exposure);

  /* 3. contrast about the pivot, on the norm so ratios (hue) hold; before the zones so a
        zone's push is judged on the final tone and not re-shaped by contrast */
  if (g.contrast != 1.0f) {
    float n = drt_grade_norm(p, rgb);
    if (n > 1e-8f) {
      float n2 = g.d_pivot_scene*_powf(n/g.d_pivot_scene, g.contrast);
      rgb = rgb*(n2/n);
    }
  }

  /* 4. the zones, with the weights from the source */
  rgb = drt_grade_apply(p, rgb, w0, g.z0_exposure, g.z0_saturation, g.z0_hue, g.z0_tint);
  rgb = drt_grade_apply(p, rgb, w1, g.z1_exposure, g.z1_saturation, g.z1_hue, g.z1_tint);
  rgb = drt_grade_apply(p, rgb, w2, g.z2_exposure, g.z2_saturation, g.z2_hue, g.z2_tint);
  rgb = drt_grade_apply(p, rgb, w3, g.z3_exposure, g.z3_saturation, g.z3_hue, g.z3_tint);
  rgb = drt_grade_apply(p, rgb, w4, g.z4_exposure, g.z4_saturation, g.z4_hue, g.z4_tint);
  rgb = drt_grade_apply(p, rgb, w5, g.z5_exposure, g.z5_saturation, g.z5_hue, g.z5_tint);

  /* 5. global saturation */
  if (g.saturation != 1.0f) {
    float y = drt_grade_luma(p, rgb);
    rgb = make_float3(y + (rgb.x - y)*g.saturation, y + (rgb.y - y)*g.saturation, y + (rgb.z - y)*g.saturation);
  }

  /* 6. the secondary's correctors through the mask from the source */
  if (g.sec_enable != 0) rgb = drt_grade_apply(p, rgb, mask, g.sec_exposure, g.sec_saturation, g.sec_hue_shift, g.sec_tint);
  return rgb;
}

#endif /* OPENDRT_GRADE_H */
