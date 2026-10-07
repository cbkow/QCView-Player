/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 *
 * Portions ported from darktable's AgX module (src/iop/agx.c and
 * src/common/custom_primaries.c), Copyright (C) 2025-2026 darktable developers,
 * GPL-3.0-or-later: the rotated / scaled primaries geometry and the sigmoid scale.
 * Modified for minColorAE (GPL §5a): double precision, D65 Rec.2020 base, the
 * primaries parameters published with Blender's AgX (see below), the HDR constants.
 */
/* minColor AgX, host side: the matrices and curve constants drt_agx() reads.
 *
 * Inset and outset primaries. Blender's AgX (Eary Chow et al.) builds them from
 * Rec.2020 at D65: each primary's ray from the white point is rotated (inset only,
 * degrees), intersected with the gamut's edges and scaled toward white by 1 - amount.
 * The amounts are the ones Blender's AgX LUT headers publish:
 *   inset rotation [2.13976149, -1.22827335, -3.05174246] degrees,
 *   inset [0.32965205, 0.28051336, 0.12475368], outset [0.32317438, 0.28325605, 0.0374326].
 * darktable's "blender-like" preset reaches the same matrices in its D50 frame; the
 * two agree within 1e-3 against Blender's views (research spike, 2026-10-04).
 */
#include "opendrt.h"

#include <cmath>

namespace drt {

namespace {

struct D3 { double m[3][3]; };

D3 mul(const D3 &a, const D3 &b)
{
    D3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

D3 inv(const D3 &a)
{
    const double (&m)[3][3] = a.m;
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    const double id = 1.0 / det;
    D3 r{};
    r.m[0][0] = c00 * id; r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id; r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    r.m[1][0] = c01 * id; r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id; r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    r.m[2][0] = c02 * id; r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id; r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    return r;
}

D3 fromCore(const drt_mat3 &c)
{
    D3 r{};
    r.m[0][0] = c.x.x; r.m[0][1] = c.x.y; r.m[0][2] = c.x.z;
    r.m[1][0] = c.y.x; r.m[1][1] = c.y.y; r.m[1][2] = c.y.z;
    r.m[2][0] = c.z.x; r.m[2][1] = c.z.y; r.m[2][2] = c.z.z;
    return r;
}

void store(float out[9], const D3 &a)
{
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) out[i * 3 + j] = float(a.m[i][j]);
}

/* RGB -> XYZ from xy primaries and white point. */
D3 npm(const double xy[3][2], const double w[2])
{
    D3 P{};
    for (int i = 0; i < 3; ++i) {
        P.m[0][i] = xy[i][0] / xy[i][1];
        P.m[1][i] = 1.0;
        P.m[2][i] = (1.0 - xy[i][0] - xy[i][1]) / xy[i][1];
    }
    const double W[3] = { w[0] / w[1], 1.0, (1.0 - w[0] - w[1]) / w[1] };
    const D3 Pi = inv(P);
    double S[3];
    for (int i = 0; i < 3; ++i) S[i] = Pi.m[i][0] * W[0] + Pi.m[i][1] * W[1] + Pi.m[i][2] * W[2];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) P.m[r][c] *= S[c];
    return P;
}

const double kR2020[3][2] = { { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 } };
const double kD65[2] = { 0.3127, 0.3290 };

/* darktable's dt_rotate_and_scale_primary: rotate primary i's ray around white, find
   where it leaves the gamut triangle, scale that distance. */
void rotateScale(double scaling, double rotation, int i, double out[2])
{
    const double wx = kD65[0], wy = kD65[1];
    const double a = std::atan2(kR2020[i][1] - wy, kR2020[i][0] - wx) + rotation;
    const double c = std::cos(a), s = std::sin(a);
    double dist = 1e30;
    for (int k = 0; k < 3; ++k) {
        const int n = k == 2 ? 0 : k + 1;
        const double x3 = kR2020[k][0], y3 = kR2020[k][1], x4 = kR2020[n][0], y4 = kR2020[n][1];
        const double den = (wx - (wx + c)) * (y3 - y4) - (x3 - x4) * (wy - (wy + s));
        if (den == 0.0) continue;
        const double t = ((wx - x3) * (y3 - y4) - (x3 - x4) * (wy - y3)) / den;
        if (t >= 0.0 && t < dist) dist = t;
    }
    out[0] = wx + scaling * dist * c;
    out[1] = wy + scaling * dist * s;
}

/* Troy Sobotka's sigmoid scale, unguarded (darktable's _scale without its clamps): the
   darkening shoulder needs the negative scale a power of 1 gives. */
double scaleRaw(double lx, double ly, double tx, double ty, double p, double slope)
{
    const double a = std::pow(slope * (lx - tx), -p);
    const double b = std::pow(slope * (lx - tx) / (ly - ty), p) - 1.0;
    return p == 1.0 ? 1.0 / (a * b) : std::pow(a * b, -1.0 / p);
}

/* The main curve's scale, guarded as darktable's _scale: a toe or shoulder the line
   cannot reach becomes nearly linear and is clamped to the curve's limits. */
double scaleGuarded(double lx, double ly, double tx, double ty, double p, double slope)
{
    const double eps = 1e-6;
    const double pr = slope * std::fmax(eps, lx - tx), ar = std::fmax(eps, ly - ty);
    const double base = std::fmax(eps, std::pow(ar, -p) - std::pow(pr, -p));
    return std::fmin(1e9, std::pow(base, -1.0 / p));
}

} // namespace

DrtAgxParams drt_agx_defaults()
{
    DrtAgxParams a = {};
    a.working_gamut = DRT_IN_AP1;
    a.target = 1.0f;          /* Rec.709 */
    a.peak = 100.0f;
    a.white_ev = 6.5f;
    a.black_ev = -10.0f;
    a.contrast = 2.4f;
    a.toe_power = 1.5f;
    a.shoulder_power = 1.5f;
    a.hue_restore = 0.6f;
    a.hdr_purity = 0.5f;
    a.outset = 1.0f;
    return a;
}

DrtAgxParams drt_agx_derive(DrtAgxParams a)
{
    const double kPi = 3.14159265358979323846;
    /* working <-> Rec.2020 base, through the core's gamut matrices (as minColor Input and
       ocio/mincolor.ocio) */
    const D3 w2x = fromCore(drt_gamut_to_xyz(int(a.working_gamut)));
    const D3 b2x = fromCore(drt_gamut_to_xyz(DRT_IN_REC2020));
    const D3 x2b = inv(b2x);
    store(a.m_wb, mul(x2b, w2x));
    store(a.m_bw, mul(inv(w2x), b2x));

    /* inset / outset, in Rec.2020 RGB coordinates (geometry on the nominal D65 primaries) */
    const D3 base = npm(kR2020, kD65), baseInv = inv(base);
    const double rot[3] = { 2.13976149 * kPi / 180.0, -1.22827335 * kPi / 180.0, -3.05174246 * kPi / 180.0 };
    const double ins[3] = { 0.32965205, 0.28051336, 0.12475368 };
    const double outs[3] = { 0.32317438, 0.28325605, 0.0374326 };
    double pin[3][2], pout[3][2];
    for (int i = 0; i < 3; ++i) {
        rotateScale(1.0 - ins[i], rot[i], i, pin[i]);
        rotateScale(1.0 - double(a.outset) * outs[i], 0.0, i, pout[i]);
    }
    store(a.m_br, mul(baseInv, npm(pin, kD65)));
    store(a.m_rb, inv(mul(baseInv, npm(pout, kD65))));

    /* target rail */
    const int tg = int(a.target + 0.5f);
    a.guard = tg == 0 ? 0.0f : 1.0f;
    const D3 t2x = fromCore(drt_gamut_to_xyz(tg == 2 ? DRT_IN_P3D65 : (tg == 1 ? DRT_IN_REC709 : DRT_IN_REC2020)));
    store(a.m_bt, mul(inv(t2x), b2x));
    store(a.m_tb, mul(inv(b2x), t2x));
    a.l_t0 = float(t2x.m[1][0]); a.l_t1 = float(t2x.m[1][1]); a.l_t2 = float(t2x.m[1][2]);

    /* curve: pivot at grey, slope scaled with the range (darktable keeps "contrast"
       meaning a 16.5-stop range); HDR shoulder power x (peak / 100)^log10(2) */
    const double range = std::fmax(1e-3, double(a.white_ev) - double(a.black_ev));
    const double px = -double(a.black_ev) / range, py = std::pow(0.18, 1.0 / 2.4);
    const double slope = double(a.contrast) * range / 16.5;
    const double ratio = std::fmax(1.0, double(a.peak) / 100.0);
    const double shp = double(a.shoulder_power) * std::pow(ratio, std::log10(2.0));
    a.range = float(range); a.px = float(px); a.py = float(py); a.slope = float(slope);
    a.sh_p = float(shp); a.ratio = float(ratio);
    a.toe_s = float(-scaleGuarded(1.0, 1.0, 1.0 - px, 1.0 - py, double(a.toe_power), slope));
    a.sh_s = float(scaleGuarded(1.0, 1.0, px, py, shp, slope));

    /* grey darkening: wide log2 domain -20 .. log2(1 / 0.18), grey pivots to grey / ratio,
       slope ~1, toe power 3, shoulder power 1 */
    const double lo = -20.0, hi = std::log2(1.0 / 0.18), dr = hi - lo;
    const double gx = -lo / dr, gy = (std::log2(1.0 / ratio) - lo) / dr;
    a.d_lo = float(lo); a.d_range = float(dr); a.d_gx = float(gx); a.d_gy = float(gy);
    a.d_toe_s = float(-scaleRaw(1.0, 1.0, 1.0 - gx, 1.0 - gy, 3.0, 1.000001));
    a.d_sh_s = float(scaleRaw(1.0, 1.0, gx, gy, 1.0, 1.000001));
    return a;
}

} // namespace drt
