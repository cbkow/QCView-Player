/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 *
 * Portions ported from darktable's AgX module (src/iop/agx.c, "inspired by Blender's
 * AgX tone mapper", by István Kovács and the darktable developers), Copyright (C)
 * 2025-2026 darktable developers, GPL-3.0-or-later: the luminance-preserving lower
 * guard rail, the log2 encoding and the toe/shoulder sigmoid (darktable's port of
 * Troy Sobotka's sigmoid), and the hue restore. Modified for minColorAE (GPL §5a):
 * rewritten in minColor's dialect-neutral style, and extended with the HDR method
 * below.
 *
 * NOT OpenDRT. "minColor AgX": AgX image formation from linear scene light to
 * linear display light, 1.0 = 100 nits, that reproduces Blender's AgX at its
 * defaults and stays parametric (range, contrast, toe, shoulder, peak).
 *
 * AgX is by Troy Sobotka (github.com/sobotka/AgX). The Blender version, whose
 * primaries and HDR construction this follows, is by Zijun Eary Zhou (Eary Chow),
 * Mark Faderbauer and Sakari Kapanen (github.com/EaryChow/AgX). The HDR method is
 * reimplemented here from its description, not copied: the shoulder power grows with
 * the peak as (peak / 100)^log10(2), and after linearising, a second sigmoid in a
 * wide log2 domain pulls mid grey down by peak / 100 while holding the top, with hue
 * and saturation half restored. At peak 100 it is the SDR AgX.
 *
 * Per pixel (drt_agx), in the AgX base gamut (Rec.2020, D65):
 *   working gamut -> Rec.2020; lower guard rail; inset matrix; log2 encode between
 *   black and white relative exposure; sigmoid; ^2.4 (display light, 1.0 = peak);
 *   [HDR: darken grey]; hue restore; outset matrix; [target guard rail into Rec.709
 *   or P3-D65, back in Rec.2020]; clip to 0..1; x peak / 100; -> working gamut.
 * drt_agx_derive() (mincolor_agx.cpp, host only) fills the matrices and curve
 * constants. Measured against Blender 5.2 (PyOpenColorIO, 2026-10-04): SDR "AgX"
 * median 0.05 %; HDR 1000 neutrals within 0.3 %, colours median 0.6 %.
 *
 * Included after opendrt_kernel.h (same dialect shims and drt_mat3 helpers).
 */

/* All floats so the C++, MSL and HLSL layouts agree: 84 scalars, 336 bytes. */
struct DrtAgxParams {
  /* ---- user : 12 ---- */
  float working_gamut;   /* DRT_IN_* of the comp's working gamut */
  float target;          /* gamut kept inside: 0 Rec.2020 (no rail), 1 Rec.709, 2 P3-D65 */
  float peak;            /* nits; 100 = SDR */
  float white_ev;        /* stops over grey reaching the top (AgX +6.5) */
  float black_ev;        /* stops under grey reaching the floor (AgX -10) */
  float contrast;        /* sigmoid slope at the pivot for a 16.5-stop range (AgX 2.4) */
  float toe_power;       /* AgX 1.5 */
  float shoulder_power;  /* AgX 1.5 at peak 100; the HDR factor multiplies it */
  float hue_restore;     /* share of the pre-curve hue kept (Blender 0.6) */
  float hdr_purity;      /* hue / saturation restored around the grey darkening (Blender 0.5) */
  float outset;          /* purity restore after the curve, 1 = Blender */
  float user_pad;

  /* ---- derived : 6 matrices, row major : 54 ---- */
  float m_wb[9];         /* working -> Rec.2020 base */
  float m_br[9];         /* base -> rendering (inset) */
  float m_rb[9];         /* rendering -> base (outset, inverted) */
  float m_bt[9];         /* base -> target gamut */
  float m_tb[9];         /* target gamut -> base */
  float m_bw[9];         /* base -> working */

  /* ---- derived : curve : 8 ---- */
  float range;           /* white_ev - black_ev */
  float px, py;          /* pivot: grey in the log domain, 0.18^(1/2.4) */
  float slope;
  float toe_s, sh_s;     /* sigmoid scales */
  float sh_p;            /* effective shoulder power (HDR factor applied) */
  float ratio;           /* peak / 100 */

  /* ---- derived : grey darkening (HDR) : 6 ---- */
  float d_gx, d_gy;      /* grey and darkened grey in the wide log domain */
  float d_toe_s, d_sh_s;
  float d_lo, d_range;   /* the wide log domain: -20 .. log2(1 / 0.18) */

  /* ---- derived : target rail : 4 ---- */
  float l_t0, l_t1, l_t2;   /* target luminance weights */
  float guard;              /* 1 = rail into the target */
};

#define DRT_AGX_SCALARS 84

/* A row-major float[9] member as a drt_mat3 (a macro: MSL cannot pass an array out of
   a constant buffer into a function). */
#define DRT_AGX_M(m) drt_make_mat3(make_float3((m)[0], (m)[1], (m)[2]), make_float3((m)[3], (m)[4], (m)[5]), make_float3((m)[6], (m)[7], (m)[8]))

/* Luminance-preserving lower guard rail (darktable's _compress_into_gamut, after
   Blender's luminance compensation): offsets negatives out, then rescales so the
   opponent-compensated luminance is kept. */
__DEVICE__ float3 drt_agx_rail(float3 p, float3 L) {
  float in_y = p.x*L.x + p.y*L.y + p.z*L.z;
  float mx = _fmaxf(p.x, _fmaxf(p.y, p.z));
  float3 opp = make_float3(mx - p.x, mx - p.y, mx - p.z);
  float y_comp = _fmaxf(opp.x, _fmaxf(opp.y, opp.z)) - (opp.x*L.x + opp.y*L.y + opp.z*L.z) + in_y;
  float off = _fmaxf(-_fminf(p.x, _fminf(p.y, p.z)), 0.0f);
  float3 ro = make_float3(p.x + off, p.y + off, p.z + off);
  float mo = _fmaxf(ro.x, _fmaxf(ro.y, ro.z));
  float3 oro = make_float3(mo - ro.x, mo - ro.y, mo - ro.z);
  float y_new = _fmaxf(oro.x, _fmaxf(oro.y, oro.z)) - (oro.x*L.x + oro.y*L.y + oro.z*L.z) + (ro.x*L.x + ro.y*L.y + ro.z*L.z);
  float k = (y_new > y_comp && y_new > 1e-6f) ? y_comp/y_new : 1.0f;
  return ro*k;
}

/* Troy Sobotka's sigmoid (darktable's _scaled_sigmoid), lengths 0. Sign-preserving
   powers: the darkening shoulder has a negative scale with power 1, which is valid. */
__DEVICE__ float drt_agx_spow(float v, float p) { return v < 0.0f ? -_powf(-v, p) : _powf(v, p); }
__DEVICE__ float drt_agx_ex(float v, float p) { return v/drt_agx_spow(1.0f + drt_agx_spow(v, p), 1.0f/p); }
__DEVICE__ float drt_agx_sig(float x, float px, float py, float slope, float toe_s, float toe_p, float sh_s, float sh_p) {
  if (x < px) return toe_s*drt_agx_ex(slope*(x - px)/toe_s, toe_p) + py;
  return sh_s*drt_agx_ex(slope*(x - px)/sh_s, sh_p) + py;
}

/* HSV hue / saturation, as darktable's dt_RGB_2_HSV / dt_HSV_2_RGB. */
__DEVICE__ float3 drt_agx_hsv(float3 c) {
  float mx = _fmaxf(c.x, _fmaxf(c.y, c.z)), mn = _fminf(c.x, _fminf(c.y, c.z)), d = mx - mn;
  if (_fabs(mx) <= 1e-6f || _fabs(d) <= 1e-6f) return make_float3(0.0f, 0.0f, mx);
  float h = c.x == mx ? (c.y - c.z)/d : (c.y == mx ? 2.0f + (c.z - c.x)/d : 4.0f + (c.x - c.y)/d);
  h = h/6.0f;
  return make_float3(h - _floorf(h), d/mx, mx);
}

__DEVICE__ float3 drt_agx_rgb(float3 hsv) {
  float C = hsv.y*hsv.z, mn = hsv.z - C;
  float h = hsv.x*6.0f, i = _floorf(h), f = h - i;
  float top = C + mn, inc = f*C + mn, dec = top - f*C;
  if (i < 0.5f) return make_float3(top, inc, mn);
  if (i < 1.5f) return make_float3(dec, top, mn);
  if (i < 2.5f) return make_float3(mn, top, inc);
  if (i < 3.5f) return make_float3(mn, dec, top);
  if (i < 4.5f) return make_float3(inc, mn, top);
  return make_float3(top, mn, dec);
}

/* Shortest-path hue interpolation, t = share of the second hue. */
__DEVICE__ float drt_agx_lerp_hue(float h1, float h2, float t) {
  float d = h2 - h1;
  if (d > 0.5f) h2 = h2 - 1.0f; else if (d < -0.5f) h2 = h2 + 1.0f;
  float r = h1 + t*(h2 - h1);
  return r - _floorf(r);
}

/* One channel: log2 encode, sigmoid, linearise. Display light, 1.0 = peak. */
__DEVICE__ float drt_agx_curve(DRT_AGX_ARG a, float v) {
  float x = (_log2f(_fmaxf(v, 1e-30f)/0.18f) - a.black_ev)/a.range;
  x = _fminf(_fmaxf(x, 0.0f), 1.0f);
  float y = drt_agx_sig(x, a.px, a.py, a.slope, a.toe_s, a.toe_power, a.sh_s, a.sh_p);
  return _powf(_fminf(_fmaxf(y, 0.0f), 1.0f), 2.4f);
}

/* HDR: pull grey down by the peak ratio in a wide log2 domain, hold the top. */
__DEVICE__ float drt_agx_darken1(DRT_AGX_ARG a, float v) {
  float x = (_log2f(_fmaxf(v, 1e-30f)/0.18f) - a.d_lo)/a.d_range;
  float y = drt_agx_sig(x, a.d_gx, a.d_gy, 1.000001f, a.d_toe_s, 3.0f, a.d_sh_s, 1.0f);
  return 0.18f*_exp2f(y*a.d_range + a.d_lo);
}

/* One pixel; a through drt_agx_derive(). Linear working gamut in and out, 1.0 = 100 nits. */
__DEVICE__ float3 drt_agx(DRT_AGX_ARG a, float3 rgb) {
  /* NaN to 0 and a +-1e6 clamp, as darktable */
  float3 c = make_float3(rgb.x == rgb.x ? rgb.x : 0.0f, rgb.y == rgb.y ? rgb.y : 0.0f, rgb.z == rgb.z ? rgb.z : 0.0f);
  c = make_float3(_fminf(_fmaxf(c.x, -1e6f), 1e6f), _fminf(_fmaxf(c.y, -1e6f), 1e6f), _fminf(_fmaxf(c.z, -1e6f), 1e6f));

  float3 b = drt_vdot(DRT_AGX_M(a.m_wb), c);
  b = drt_agx_rail(b, make_float3(0.2658180370250449f, 0.59846986045365f, 0.1357121025213052f));
  float3 r = drt_vdot(DRT_AGX_M(a.m_br), b);
  float h_pre = drt_agx_hsv(r).x;

  float3 y = make_float3(drt_agx_curve(a, r.x), drt_agx_curve(a, r.y), drt_agx_curve(a, r.z));

  if (a.ratio > 1.0f) {
    float3 h0 = drt_agx_hsv(y);
    float3 dk = make_float3(drt_agx_darken1(a, y.x), drt_agx_darken1(a, y.y), drt_agx_darken1(a, y.z));
    float3 h1 = drt_agx_hsv(dk);
    h1.x = drt_agx_lerp_hue(h0.x, h1.x, a.hdr_purity);
    h1.y = h0.y + a.hdr_purity*(h1.y - h0.y);
    y = drt_agx_rgb(h1);
  }

  float3 hs = drt_agx_hsv(y);
  hs.x = drt_agx_lerp_hue(h_pre, hs.x, 1.0f - a.hue_restore);
  y = drt_agx_rgb(hs);

  float3 o = drt_vdot(DRT_AGX_M(a.m_rb), y);              /* base, display light, 1.0 = peak */
  if (a.guard > 0.5f)                                       /* keep it inside the target gamut */
    o = drt_vdot(DRT_AGX_M(a.m_tb), drt_agx_rail(drt_vdot(DRT_AGX_M(a.m_bt), o), make_float3(a.l_t0, a.l_t1, a.l_t2)));
  o = make_float3(_fminf(_fmaxf(o.x, 0.0f), 1.0f), _fminf(_fmaxf(o.y, 0.0f), 1.0f), _fminf(_fmaxf(o.z, 0.0f), 1.0f))*a.ratio;
  return drt_vdot(DRT_AGX_M(a.m_bw), o);
}
