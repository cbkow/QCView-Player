/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 *
 * NOT DERIVED FROM OPENDRT. The "minColor Knee" effect: a highlight knee for
 * ultrabright linear light, ported from QCView's linear stage (its Highlight Knee,
 * QCView-Player src/color/linear_stage.h, by the same author).
 *
 * A shoulder applied in PQ to max(R,G,B), all three channels scaled by the same
 * ratio so hue holds. It compresses [knee start, source peak] onto [knee start,
 * target peak]; values at or above the source peak land on the target, values below
 * the knee start are untouched (exact identity).
 *
 * Two shoulders. Auto: BT.2390's Hermite with BT.2390's knee start, exactly as QCView
 * (a 1000 -> 100 nits knee then starts near 28 nits and SDR white lands near 70).
 * A hand-set Knee Start uses a hyperbola instead: the Hermite overshoots the target
 * when it starts above BT.2390's point (1000 -> 100 starting at 0.9 put 200 nits at
 * 110), the hyperbola cannot. In PQ, normalised to the source, with x the position
 * past the knee start and s = (1 - ks) / (target - ks):
 *     f(x) = s x / (1 + (s - 1) x)
 * f(0) = 0 with slope s, so the curve leaves the identity line without a kink;
 * f(1) = 1, so the source peak lands on the target; f' > 0 throughout (monotonic).
 *
 * Linear in and out, 1.0 = 100 nits (the convention of QCView and ocio/mincolor.ocio).
 *
 * No gamut: max(R,G,B) is taken over the pixels as they arrive, in whatever linear
 * space the comp is in. The scale factor is gamut-independent; only which channel is
 * the max depends on the primaries, so neutrals compress identically in any working
 * space and saturated highlights differ slightly (QCView measures in Rec.2020).
 *
 * Fields (spare DrtParams slots, so the struct and its Metal layout do not change):
 *   kn_src, kn_tgt  source and target peak, nits
 *   kn_auto         1 = BT.2390's knee start; 0 = kn_start
 *   kn_start        knee start as a fraction of the target in PQ (0..1)
 *   kn_pq_src, kn_max_lum   derived by drt_knee_derive(): PQ(source), PQ(target)/PQ(source)
 * Included after opendrt_kernel.h (same dialect shims).
 */

/* ST 2084. y = luminance / 10000 nits. */
__DEVICE__ float drt_knee_pq_enc(float y) {
  const float m1 = 0.1593017578125f, m2 = 78.84375f;
  const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
  float p = _powf(_fmaxf(y, 0.0f), m1);
  return _powf((c1 + c2*p)/(1.0f + c3*p), m2);
}

__DEVICE__ float drt_knee_pq_dec(float e) {
  const float m1 = 0.1593017578125f, m2 = 78.84375f;
  const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
  float p = _powf(_fminf(_fmaxf(e, 0.0f), 1.0f), 1.0f/m2);
  return _powf(_fmaxf(p - c1, 0.0f)/(c2 - c3*p), 1.0f/m1);
}

/* Host side, once per frame: the PQ constants the pixel function needs. */
__DEVICE__ DrtParams drt_knee_derive(DrtParams p) {
  float src = _fmaxf(p.kn_src, 1.0f);
  float tgt = _fmaxf(p.kn_tgt, 1.0f);
  p.kn_pq_src  = drt_knee_pq_enc(src/10000.0f);
  p.kn_max_lum = drt_knee_pq_enc(tgt/10000.0f)/p.kn_pq_src;
  return p;
}

/* The knee start in PQ, normalised to the source (BT.2390's default, or the user's). */
__DEVICE__ float drt_knee_start(DRT_PARAMS_ARG p) {
  float ml = p.kn_max_lum;
  float ks = p.kn_auto != 0 ? _fmaxf(0.0f, 1.5f*ml - 0.5f) : p.kn_start*ml;
  return _fminf(_fmaxf(ks, 0.0f), ml*0.999f);
}

/* One pixel; p through drt_knee_derive(). */
__DEVICE__ float3 drt_knee(DRT_PARAMS_ARG p, float3 rgb) {
  float ml = p.kn_max_lum;
  if (ml >= 0.999f) return rgb;                  /* a target at or above the source: nothing to compress */
  float m = _fmaxf(rgb.x, _fmaxf(rgb.y, rgb.z));
  if (m <= 0.0f) return rgb;
  float e1 = _fminf(drt_knee_pq_enc(m*0.01f)/p.kn_pq_src, 1.0f);
  float ks = drt_knee_start(p);
  if (e1 < ks) return rgb;                       /* below the knee: untouched */
  float t = (e1 - ks)/(1.0f - ks);
  float e2;
  if (p.kn_auto != 0) {                          /* BT.2390 Hermite (QCView's) */
    float t2 = t*t, t3 = t2*t;
    e2 = (2.0f*t3 - 3.0f*t2 + 1.0f)*ks + (t3 - 2.0f*t2 + t)*(1.0f - ks) + (-2.0f*t3 + 3.0f*t2)*ml;
  } else {                                       /* hand-set start: monotonic hyperbola */
    float s = (1.0f - ks)/(ml - ks);
    e2 = ks + (ml - ks)*(s*t/(1.0f + (s - 1.0f)*t));
  }
  float k = drt_knee_pq_dec(e2*p.kn_pq_src)*100.0f/m;
  return rgb*k;
}
