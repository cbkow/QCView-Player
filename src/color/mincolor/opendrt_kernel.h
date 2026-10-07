/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 * Derived from OpenDRT v1.1.0 by Jed Smith (https://github.com/jedypod/open-display-transform), GPLv3;
 * modified 2026-09-30 to 2026-10-04, see CHANGES-FROM-OPENDRT.md. Not affiliated with or endorsed by OpenDRT.
 */
/* OpenDRT core kernel — dialect-neutral port of OpenDRT v1.1.0.
 *
 * Original: OpenDRT.dctl, written by Jed Smith
 *   https://github.com/jedypod/open-display-transform
 *   License: GPL-3.0. This port keeps that license. See LICENSE.
 *
 * WHAT THIS FILE IS
 *   The whole per-pixel picture formation, in one source that compiles as C++, MSL,
 *   HLSL and GLSL once a shim (opendrt_shim_<dialect>.h) is included before it.
 *   The math is the upstream DCTL body, transcribed line for line; only three things
 *   changed, and every one of them is a mechanical rule so the next upstream release
 *   can be diffed against this file:
 *     1. Parameters come from a DrtParams struct (opendrt_params.h) instead of DCTL
 *        globals, and the preset resolution that the DCTL does at the top of
 *        transform() moved to the host (opendrt_presets.cpp). The kernel only ever
 *        sees fully resolved values.
 *     2. Per-render constants (tonescale constraints, contrast low/high constants,
 *        input matrix, render-space weights) are computed once in drt_derive() and
 *        read back from the struct, instead of being recomputed per pixel.
 *        The expressions are identical, in the same order, so the floats match.
 *     3. Every function and constant carries a drt_ prefix so the kernel can be
 *        concatenated into a host shader (another host pastes it next to OCIO's emitted
 *        code) without name collisions. Rename table for diffing against upstream:
 *          make_float3x3 -> drt_make_mat3     float3x3 -> drt_mat3   identity -> drt_identity
 *          vdot -> drt_vdot   sdivf -> drt_sdivf   spowf -> drt_spowf   (and so on: name -> drt_name)
 *          PI -> DRT_PI       SQRT3 -> DRT_SQRT3   transform -> drt_transform
 *   Dropped on purpose: the tonescale overlay (crv_enable), the only thing in the
 *   DCTL that reads pixel coordinates. Hosts that want a curve plot can draw one
 *   from drt_transform() on a ramp.
 *
 * THE DIALECT
 *   The subset is DCTL's own: float / float2 / float3 with component-wise
 *   operators, make_float2 / make_float3, the _xxxf intrinsic names, __DEVICE__ on
 *   functions and __CONSTANT__ on globals. Rules that keep it compiling everywhere
 *   (GLSL is the strictest, so it sets them):
 *     - if() takes a bool: write `if (p.flag != 0)`, never `if (p.flag)`.
 *     - casts are functional: `float(i)`, never `(float)i`.
 *     - no `const` on locals, no typedef, no enum, no pointers, no ternary on structs.
 *     - float literals carry the f suffix; no double anywhere.
 *     - _fmod means C fmod (truncation). GLSL's mod() floors, so the GLSL shim
 *        defines _fmod by hand. It matters: drt_hue_offset feeds it negative values.
 *     - struct members are declared one per line.
 *   A shim must define: float2, float3, make_float2, make_float3, __DEVICE__,
 *   __CONSTANT__, DRT_PARAMS_ARG, and _powf _expf _exp2f _exp10f _logf _log2f
 *   _sqrtf _fabs _fmod _atan2f _fminf _fmaxf. opendrt_params.h must be included
 *   before this file.
 */
#ifndef OPENDRT_KERNEL_H
#define OPENDRT_KERNEL_H

__CONSTANT__ float DRT_SQRT3 = 1.73205080756887729353f;
__CONSTANT__ float DRT_PI = 3.14159265358979323846f;

/* ---------------------------------------------------------------- 3x3 matrix */

struct drt_mat3 {
  float3 x;
  float3 y;
  float3 z;
};

__DEVICE__ drt_mat3 drt_make_mat3(float3 a, float3 b, float3 c) {
  drt_mat3 d;
  d.x = a;
  d.y = b;
  d.z = c;
  return d;
}

__DEVICE__ drt_mat3 drt_identity() {
  return drt_make_mat3(make_float3(1.0f, 0.0f, 0.0f), make_float3(0.0f, 1.0f, 0.0f), make_float3(0.0f, 0.0f, 1.0f));
}

/* Multiply 3x3 matrix m and float3 vector v */
__DEVICE__ float3 drt_vdot(drt_mat3 m, float3 v) {
  return make_float3(m.x.x*v.x + m.x.y*v.y + m.x.z*v.z, m.y.x*v.x + m.y.y*v.y + m.y.z*v.z, m.z.x*v.x + m.z.y*v.y + m.z.z*v.z);
}

/* Inverse of a 3x3 (cofactors). Not in upstream: used once per parameter change
   by drt_derive() to turn a gamut's forward matrix into its XYZ -> gamut inverse. */
__DEVICE__ drt_mat3 drt_mat3_inverse(drt_mat3 m) {
  float c00 = m.y.y*m.z.z - m.y.z*m.z.y;
  float c01 = m.y.z*m.z.x - m.y.x*m.z.z;
  float c02 = m.y.x*m.z.y - m.y.y*m.z.x;
  float det = m.x.x*c00 + m.x.y*c01 + m.x.z*c02;
  if (det == 0.0f) return drt_identity();
  float id = 1.0f/det;
  return drt_make_mat3(
    make_float3(c00*id, (m.x.z*m.z.y - m.x.y*m.z.z)*id, (m.x.y*m.y.z - m.x.z*m.y.y)*id),
    make_float3(c01*id, (m.x.x*m.z.z - m.x.z*m.z.x)*id, (m.x.z*m.y.x - m.x.x*m.y.z)*id),
    make_float3(c02*id, (m.x.y*m.z.x - m.x.x*m.z.y)*id, (m.x.x*m.y.y - m.x.y*m.y.x)*id));
}

/* ------------------------------------------------ gamut conversion matrices */
/* Kept as macros like upstream: constructed at the use site, folded by the compiler. */

/* ACES AP0 to XYZ D65 (CAT02) */
#define drt_matrix_ap0_to_xyz drt_make_mat3(make_float3(0.938630948750273197f, -0.00574192055037397141f, 0.017566898851772296f), make_float3(0.338093594922021567f, 0.72721390281143572f, -0.0653074977334571899f), make_float3(0.000723121511341165988f, 0.000818441849244731985f, 1.08751618739929268f))
/* ACES AP1 to XYZ D65 (CAT02) */
#define drt_matrix_ap1_to_xyz drt_make_mat3(make_float3(0.652418717671912951f, 0.127179925537538263f, 0.170857283842220459f), make_float3(0.268064059194271287f, 0.672464478992617742f, 0.0594714618131108388f), make_float3(-0.0054699285104975676f, 0.00518279997697511721f, 1.08934487929340107f))
#define drt_matrix_rec2020_to_xyz drt_make_mat3(make_float3(0.636958048301290991f, 0.144616903586208406f, 0.168880975164172054f), make_float3(0.26270021201126692f, 0.677998071518871148f, 0.0593017164698619384f), make_float3(4.9999999999999999e-17f, 0.0280726930490874452f, 1.06098505771079066f))
#define drt_matrix_arriwg3_to_xyz drt_make_mat3(make_float3(0.638007619284f, 0.214703856337f, 0.097744451431f), make_float3(0.291953779f, 0.823841041511f, -0.11579482051f), make_float3(0.002798279032f, -0.067034235689f, 1.15329370742f))
#define drt_matrix_arriwg4_to_xyz drt_make_mat3(make_float3(0.704858320407231953f, 0.129760295170463003f, 0.115837311473976537f), make_float3(0.254524176404026969f, 0.781477732712002049f, -0.0360019091160290391f), make_float3(0.0f, 0.0f, 1.08905775075987843f))
#define drt_matrix_redwg_to_xyz drt_make_mat3(make_float3(0.735275245905858799f, 0.0686094106139610721f, 0.14657127053185201f), make_float3(0.286694099499934962f, 0.842979134016975662f, -0.129673233516910319f), make_float3(-0.0796808568783676785f, -0.347343216994429771f, 1.51608182463267593f))
#define drt_matrix_sonysgamut3_to_xyz drt_make_mat3(make_float3(0.706482713192318812f, 0.12880104979055762f, 0.115172164068795255f), make_float3(0.270979670813492168f, 0.786606411220905466f, -0.0575860820343976273f), make_float3(-0.00967784538619615754f, 0.00460003749251991934f, 1.09413555865355483f))
#define drt_matrix_sonysgamut3cine_to_xyz drt_make_mat3(make_float3(0.599083920758327171f, 0.248925516115423628f, 0.102446490177920776f), make_float3(0.215075820115587457f, 0.88506850174372842f, -0.100144321859315821f), make_float3(-0.0320658495445057951f, -0.0276583906794915374f, 1.1487819909838759f))
#define drt_matrix_vgamut_to_xyz drt_make_mat3(make_float3(0.679644469878f, 0.15221141244f, 0.118600044733f), make_float3(0.26068555009f, 0.77489446333f, -0.03558001342f), make_float3(-0.009310198218f, -0.004612467044f, 1.10298041602f))
#define drt_matrix_egamut_to_xyz drt_make_mat3(make_float3(0.705396850087770755f, 0.164041328309919021f, 0.0810177486539819941f), make_float3(0.280130724091105898f, 0.820206641549595106f, -0.100337365640700782f), make_float3(-0.103781511569163279f, -0.0729072570266306313f, 1.26574651935567273f))
#define drt_matrix_egamut2_to_xyz drt_make_mat3(make_float3(0.736477700183697404f, 0.130739651086660136f, 0.0832385757813140781f), make_float3(0.275069984405959256f, 0.828017790215514138f, -0.103087774621473588f), make_float3(-0.124225154247852534f, -0.0871597673911067433f, 1.30044267239883782f))
#define drt_matrix_davinciwg_to_xyz drt_make_mat3(make_float3(0.700622392093671609f, 0.148774815123196763f, 0.101058719834803246f), make_float3(0.274118510906649016f, 0.873631895940436665f, -0.147750406847085763f), make_float3(-0.0989629128832311411f, -0.137895325075543307f, 1.32591598871865268f))

/* P3D65 to XYZ D65 */
#define drt_matrix_p3d65_to_xyz drt_make_mat3(make_float3(0.486570948648216151f, 0.265667693169093f, 0.198217285234362467f), make_float3(0.228974564069748754f, 0.691738521836506193f, 0.079286914093744984f), make_float3(-4.00000000000000029e-17f, 0.0451133818589026167f, 1.04394436890097575f))
/* XYZ D65 to P3D65 */
#define drt_matrix_xyz_to_p3d65 drt_make_mat3(make_float3(2.49349691194142542f, -0.93138361791912383f, -0.402710784450716841f), make_float3(-0.829488969561574696f, 1.76266406031834655f, 0.0236246858419435941f), make_float3(0.0358458302437844531f, -0.0761723892680418041f, 0.956884524007687309f))
/* Rec709 to XYZ D65 */
#define drt_matrix_rec709_to_xyz drt_make_mat3(make_float3(0.412390799265959229f, 0.357584339383878125f, 0.180480788401834347f), make_float3(0.212639005871510217f, 0.71516867876775625f, 0.0721923153607337414f), make_float3(0.0193308187155918181f, 0.119194779794626018f, 0.950532152249661033f))
/* XYZ D65 to Rec709 */
#define drt_matrix_xyz_to_rec709 drt_make_mat3(make_float3(3.24096994190452348f, -1.53738317757009435f, -0.498610760293003552f), make_float3(-0.969243636280879506f, 1.87596750150771996f, 0.0415550574071755843f), make_float3(0.0556300796969936354f, -0.20397695888897649f, 1.05697151424287816f))
/* P3D65 to Rec2020 */
#define drt_matrix_p3_to_rec2020 drt_make_mat3(make_float3(0.753833034361722221f, 0.198597369052616435f, 0.0475695965856618441f), make_float3(0.0457438489653582137f, 0.9417772198116936f, 0.0124789312229481135f), make_float3(-0.0012103403545183941f, 0.0176017173010899926f, 0.983608623053428777f))
/* P3DCI to XYZ DCI (NPM Matrix) — unused by v1.1.0 as by upstream, kept for parity */
#define drt_matrix_p3dci_to_xyz drt_make_mat3(make_float3(0.445169815564552429f, 0.27713440920677751f, 0.172282669815564504f), make_float3(0.209491677912730545f, 0.721595254161043309f, 0.0689130679262257989f), make_float3(-3.59999999999999995e-17f, 0.0470605600539811264f, 0.9073553943619731f))

/* CAT02 chromatic adaptation matrices */
/* DCI to D93 : [0.314, 0.351] to [0.283, 0.297] */
#define drt_matrix_cat_dci_to_d93 drt_make_mat3(make_float3(0.965685009956359863f, 0.00183745240792632103f, 0.0912967324256896973f), make_float3(0.000514572137035429044f, 0.965166747570037842f, 0.036014653742313385f), make_float3(0.00154250487685203596f, 0.00702651776373386383f, 1.47287476062774658f))
/* DCI to D75 : [0.314, 0.351] to [0.29903, 0.31488] */
#define drt_matrix_cat_dci_to_d75 drt_make_mat3(make_float3(0.990120768547058105f, 0.0151389474049210548f, 0.0511047691106796265f), make_float3(0.0102197211235761642f, 0.971718132495880127f, 0.0200536623597145081f), make_float3(0.000743072712793946049f, 0.0042176349088549614f, 1.27959656715393066f))
/* DCI to D65 : [0.314, 0.351] to [0.3127, 0.329] */
#define drt_matrix_cat_dci_to_d65 drt_make_mat3(make_float3(1.00951600074768066f, 0.0269675441086292267f, 0.0213620811700820923f), make_float3(0.0187991037964820862f, 0.975330352783203125f, 0.0082273334264755249f), make_float3(0.000134543282911180989f, 0.00217903498560190201f, 1.138663649559021f))
/* DCI to D60 : [0.314, 0.351] to [0.32162624, 0.337737] */
#define drt_matrix_cat_dci_to_d60 drt_make_mat3(make_float3(1.02159523963928223f, 0.034848678857088089f, 0.00371252000331878705f), make_float3(0.0244968775659799576f, 0.976937234401702881f, 0.00120301544666290305f), make_float3(-0.00023391586728393999f, 0.000986687839031219049f, 1.05594265460968018f))
/* DCI to D55 : [0.314, 0.351] to [0.33243, 0.34744] */
#define drt_matrix_cat_dci_to_d55 drt_make_mat3(make_float3(1.03594577312469482f, 0.0450937561690807343f, -0.0157573819160461426f), make_float3(0.0318740680813789368f, 0.977744519710540771f, -0.00655744969844818115f), make_float3(-0.000653609400615095984f, -0.000297372229397297014f, 0.966327786445617676f))
/* DCI to D50 : [0.314, 0.351] to [0.3457, 0.3585] */
#define drt_matrix_cat_dci_to_d50 drt_make_mat3(make_float3(1.05306875705718994f, 0.0581297315657138824f, -0.0376100838184356689f), make_float3(0.0412359423935413361f, 0.977693676948547363f, -0.0152792222797870636f), make_float3(-0.00113777676597237609f, -0.00170759297907352404f, 0.867368340492248535f))

/* D65 to D93 : [0.3127, 0.329] to [0.283, 0.297] */
#define drt_matrix_cat_d65_to_d93 drt_make_mat3(make_float3(0.95703423023223877f, -0.0247171502560377121f, 0.0624028593301773071f), make_float3(-0.0179296955466270447f, 0.990019857883453369f, 0.0248119533061981201f), make_float3(0.00127589143812656403f, 0.00427919067442417058f, 1.29345715045928955f))
/* D65 to D75 : [0.3127, 0.329] to [0.29903, 0.31488] */
#define drt_matrix_cat_d65_to_d75 drt_make_mat3(make_float3(0.981001079082489014f, -0.0116619253531098366f, 0.0265614092350006104f), make_float3(-0.00843488052487373352f, 0.996506094932556152f, 0.0105696544051170349f), make_float3(0.000552809564396739006f, 0.00179840810596942902f, 1.12374722957611084f))
/* D65 to D60 : [0.3127, 0.329] to [0.32162624, 0.337737] */
#define drt_matrix_cat_d65_to_d60 drt_make_mat3(make_float3(1.01182246208190918f, 0.00778879318386316299f, -0.0157783031463623047f), make_float3(0.00561682833358645439f, 1.00150644779205322f, -0.00628517568111419678f), make_float3(-0.000335735734552145004f, -0.0010509500280022619f, 0.927366673946380615f))
/* D65 to D55 : [0.3127, 0.329] to [0.33243, 0.34744] */
#define drt_matrix_cat_d65_to_d55 drt_make_mat3(make_float3(1.02585089206695557f, 0.0179439820349216461f, -0.0332137793302536011f), make_float3(0.0129133854061365128f, 1.00214779376983643f, -0.0132421031594276428f), make_float3(-0.000719940289855003032f, -0.00218106806278228803f, 0.84868013858795166f))
/* D65 to D50 : [0.3127, 0.329] to [0.3457, 0.3585] */
#define drt_matrix_cat_d65_to_d50 drt_make_mat3(make_float3(1.04257404804229736f, 0.03089117631316185f, -0.052812620997428894f), make_float3(0.0221935361623764038f, 1.00185668468475342f, -0.0210737623274326324f), make_float3(-0.00116488314233720303f, -0.00342052709311246915f, 0.761789083480834961f))
/* D65 to DCI-P3 : [0.3127, 0.329] to [0.314, 0.351] */
#define drt_matrix_cat_d65_to_dci drt_make_mat3(make_float3(0.991085588932037354f, -0.0273622870445251465f, -0.0183956623077392578f), make_float3(-0.0191021915525197983f, 1.02583777904510498f, -0.00705372542142868042f), make_float3(-8.05503223091359977e-05f, -0.00195988826453685804f, 0.878238439559936523f))

/* D60 to D93 : [0.32162624, 0.337737] to [0.283, 0.297] */
#define drt_matrix_cat_d60_to_d93 drt_make_mat3(make_float3(0.946056902408599854f, -0.0319503024220466614f, 0.0831701457500457764f), make_float3(-0.0231979694217443466f, 0.988745808601379395f, 0.0330617502331733704f), make_float3(0.00169203430414199807f, 0.00572328735142946243f, 1.39483106136322021f))
/* D60 to D75 : [0.32162624, 0.337737] to [0.29903, 0.31488] */
#define drt_matrix_cat_d60_to_d75 drt_make_mat3(make_float3(0.969659984111785889f, -0.019138311967253685f, 0.0450099557638168335f), make_float3(-0.0138545772060751915f, 0.995133817195892334f, 0.0179062262177467346f), make_float3(0.000931452261283994046f, 0.00306008197367191315f, 1.21179807186126709f))
/* D60 to D65 : [0.32162624, 0.337737] to [0.3127, 0.329] */
#define drt_matrix_cat_d60_to_d65 drt_make_mat3(make_float3(0.988363921642303467f, -0.00766910053789615631f, 0.0167641639709472656f), make_float3(-0.0055409618653357029f, 0.998546123504638672f, 0.00667332112789154139f), make_float3(0.000351537019014359008f, 0.00112883746623992898f, 1.07833576202392578f))
/* D60 to D55 : [0.32162624, 0.337737] to [0.33243, 0.34744] */
#define drt_matrix_cat_d60_to_d55 drt_make_mat3(make_float3(1.01380288600921631f, 0.0100131509825587273f, -0.018498346209526062f), make_float3(0.00720565160736441612f, 1.00057685375213623f, -0.00737529993057250977f), make_float3(-0.000401133671402930997f, -0.00121434964239597299f, 0.915135681629180908f))
/* D60 to D50 : [0.32162624, 0.337737] to [0.3457, 0.3585] */
#define drt_matrix_cat_d60_to_d50 drt_make_mat3(make_float3(1.03025269508361816f, 0.0227910466492176056f, -0.0392656922340393066f), make_float3(0.0163766480982303619f, 1.00020599365234375f, -0.0156668238341808319f), make_float3(-0.000864576781168579947f, -0.00254668481647968292f, 0.821422040462493896f))

/* Forward matrix of a DRT_IN_* gamut (gamut -> XYZ D65). Not in upstream as a
   function; it is the in_gamut branch of transform(), shared with the working
   gamut of drt_input_transform(). */
__DEVICE__ drt_mat3 drt_gamut_to_xyz(int gamut) {
  drt_mat3 m;
  if (gamut == 1) m = drt_matrix_ap0_to_xyz;
  else if (gamut == 2) m = drt_matrix_ap1_to_xyz;
  else if (gamut == 3) m = drt_matrix_p3d65_to_xyz;
  else if (gamut == 4) m = drt_matrix_rec2020_to_xyz;
  else if (gamut == 5) m = drt_matrix_rec709_to_xyz;
  else if (gamut == 6) m = drt_matrix_arriwg3_to_xyz;
  else if (gamut == 7) m = drt_matrix_arriwg4_to_xyz;
  else if (gamut == 8) m = drt_matrix_redwg_to_xyz;
  else if (gamut == 9) m = drt_matrix_sonysgamut3_to_xyz;
  else if (gamut == 10) m = drt_matrix_sonysgamut3cine_to_xyz;
  else if (gamut == 11) m = drt_matrix_vgamut_to_xyz;
  else if (gamut == 12) m = drt_matrix_egamut_to_xyz;
  else if (gamut == 13) m = drt_matrix_egamut2_to_xyz;
  else if (gamut == 14) m = drt_matrix_davinciwg_to_xyz;
  else m = drt_identity();  /* 0 = XYZ, and anything out of range */
  return m;
}

/* ------------------------------------------------------------ math helpers */

/* Safe division of float a by float b */
__DEVICE__ float drt_sdivf(float a, float b) {
  if (b == 0.0f) return 0.0f;
  else return a/b;
}

/* Safe division of float3 a by float b */
__DEVICE__ float3 drt_sdivf3f(float3 a, float b) {
  return make_float3(drt_sdivf(a.x, b), drt_sdivf(a.y, b), drt_sdivf(a.z, b));
}

/* Safe power function raising float a to power float b */
__DEVICE__ float drt_spowf(float a, float b) {
  if (a <= 0.0f) return a;
  else return _powf(a, b);
}

/* Safe power function raising float3 a to power float b */
__DEVICE__ float3 drt_spowf3(float3 a, float b) {
  return make_float3(drt_spowf(a.x, b), drt_spowf(a.y, b), drt_spowf(a.z, b));
}

/* Return the hypot or vector length of float2 v */
__DEVICE__ float drt_hypotf2(float2 v) { return _sqrtf(_fmaxf(0.0f, v.x*v.x + v.y*v.y)); }

/* Return the hypot or vector length of float3 v */
__DEVICE__ float drt_hypotf3(float3 v) { return _sqrtf(_fmaxf(0.0f, v.x*v.x + v.y*v.y + v.z*v.z)); }

/* Return the max of float3 a */
__DEVICE__ float drt_fmaxf3_(float3 a) { return _fmaxf(a.x, _fmaxf(a.y, a.z)); }

/* Clamp float3 a to min value mn */
__DEVICE__ float3 drt_clampminf3(float3 a, float mn) { return make_float3(_fmaxf(a.x, mn), _fmaxf(a.y, mn), _fmaxf(a.z, mn)); }

/* Clamp float a to min value mn and max value mx */
__DEVICE__ float drt_clampf(float a, float mn, float mx) { return _fminf(_fmaxf(a, mn), mx); }
__DEVICE__ float3 drt_clampf3(float3 a, float mn, float mx) { return make_float3(drt_clampf(a.x, mn, mx), drt_clampf(a.y, mn, mx), drt_clampf(a.z, mn, mx)); }

/* ------------------------------------- OETF linearization transfer functions */

__DEVICE__ float drt_oetf_davinci_intermediate(float x) {
  return x <= 0.02740668f ? x/10.44426855f : _exp2f(x/0.07329248f - 7.0f) - 0.0075f;
}
__DEVICE__ float drt_oetf_filmlight_tlog(float x) {
  return x < 0.075f ? (x-0.075f)/16.184376489665897f : _expf((x - 0.5520126568606655f)/0.09232902596577353f) - 0.0057048244042473785f;
}
__DEVICE__ float drt_oetf_acescct(float x) {
  return x <= 0.155251141552511f ? (x - 0.0729055341958355f)/10.5402377416545f : _exp2f(x*17.52f - 9.72f);
}
__DEVICE__ float drt_oetf_arri_logc3(float x) {
  return x < 5.367655f*0.010591f + 0.092809f ? (x - 0.092809f)/5.367655f : (_exp10f((x - 0.385537f)/0.247190f) - 0.052272f)/5.555556f;
}
__DEVICE__ float drt_oetf_arri_logc4(float x) {
  return x < -0.7774983977293537f ? x*0.3033266726886969f - 0.7774983977293537f : (_exp2f(14.0f*(x - 0.09286412512218964f)/0.9071358748778103f + 6.0f) - 64.0f)/2231.8263090676883f;
}
__DEVICE__ float drt_oetf_red_log3g10(float x) {
  return x < 0.0f ? (x/15.1927f) - 0.01f : (_exp10f(x/0.224282f) - 1.0f)/155.975327f - 0.01f;
}
__DEVICE__ float drt_oetf_panasonic_vlog(float x) {
  return x < 0.181f ? (x - 0.125f)/5.6f : _exp10f((x - 0.598206f)/0.241514f) - 0.00873f;
}
__DEVICE__ float drt_oetf_sony_slog3(float x) {
  return x < 171.2102946929f/1023.0f ? (x*1023.0f - 95.0f)*0.01125f/(171.2102946929f - 95.0f) : (_exp10f(((x*1023.0f - 420.0f)/261.5f))*(0.18f + 0.01f) - 0.01f);
}
__DEVICE__ float drt_oetf_fujifilm_flog2(float x) {
  return x < 0.100686685370811f ? (x - 0.092864f)/8.799461f : (_exp10f(((x - 0.384316f)/0.245281f))/5.555556f - 0.064829f/5.555556f);
}

/* Display-referred decodes (not in upstream; see DRT_OETF_REC1886 and friends).
   Sign-preserving so slightly negative code values do not turn into NaN. */
__DEVICE__ float drt_eotf_power(float x, float p) {
  return x < 0.0f ? -_powf(-x, p) : _powf(x, p);
}
__DEVICE__ float drt_eotf_srgb(float x) {
  float a = x < 0.0f ? -x : x;
  float y = a <= 0.04045f ? a/12.92f : _powf((a + 0.055f)/1.055f, 2.4f);
  return x < 0.0f ? -y : y;
}
__DEVICE__ float drt_eotf_bt709_camera(float x) {
  float a = x < 0.0f ? -x : x;
  float y = a < 0.081f ? a/4.5f : _powf((a + 0.099f)/1.099f, 1.0f/0.45f);
  return x < 0.0f ? -y : y;
}

__DEVICE__ float3 drt_eotf_hlg(float3 rgb, int inverse);
__DEVICE__ float3 drt_eotf_pq(float3 rgb, int inverse);

__DEVICE__ float3 drt_linearize(float3 rgb, int tf) {
  if (tf == 0) { /* Linear */
    return rgb;
  } else if (tf == 10) { /* Rec.1886, 2.4 power */
    rgb = make_float3(drt_eotf_power(rgb.x, 2.4f), drt_eotf_power(rgb.y, 2.4f), drt_eotf_power(rgb.z, 2.4f));
  } else if (tf == 11) { /* sRGB piecewise */
    rgb = make_float3(drt_eotf_srgb(rgb.x), drt_eotf_srgb(rgb.y), drt_eotf_srgb(rgb.z));
  } else if (tf == 12) { /* 2.2 power */
    rgb = make_float3(drt_eotf_power(rgb.x, 2.2f), drt_eotf_power(rgb.y, 2.2f), drt_eotf_power(rgb.z, 2.2f));
  } else if (tf == 13) { /* BT.709 camera OETF inverse */
    rgb = make_float3(drt_eotf_bt709_camera(rgb.x), drt_eotf_bt709_camera(rgb.y), drt_eotf_bt709_camera(rgb.z));
  } else if (tf == 14) { /* PQ, 100 nits = 1.0 */
    rgb = drt_eotf_pq(rgb, 0)*100.0f;
  } else if (tf == 15) { /* HLG 1000 nits, 100 nits = 1.0 */
    rgb = drt_eotf_hlg(rgb, 0)*10.0f;
  } else if (tf == 1) { /* Davinci Intermediate */
    rgb.x = drt_oetf_davinci_intermediate(rgb.x);
    rgb.y = drt_oetf_davinci_intermediate(rgb.y);
    rgb.z = drt_oetf_davinci_intermediate(rgb.z);
  } else if (tf == 2) { /* Filmlight T-Log */
    rgb.x = drt_oetf_filmlight_tlog(rgb.x);
    rgb.y = drt_oetf_filmlight_tlog(rgb.y);
    rgb.z = drt_oetf_filmlight_tlog(rgb.z);
  } else if (tf == 3) { /* ACEScct */
    rgb.x = drt_oetf_acescct(rgb.x);
    rgb.y = drt_oetf_acescct(rgb.y);
    rgb.z = drt_oetf_acescct(rgb.z);
  } else if (tf == 4) { /* Arri LogC3 */
    rgb.x = drt_oetf_arri_logc3(rgb.x);
    rgb.y = drt_oetf_arri_logc3(rgb.y);
    rgb.z = drt_oetf_arri_logc3(rgb.z);
  } else if (tf == 5) { /* Arri LogC4 */
    rgb.x = drt_oetf_arri_logc4(rgb.x);
    rgb.y = drt_oetf_arri_logc4(rgb.y);
    rgb.z = drt_oetf_arri_logc4(rgb.z);
  } else if (tf == 6) { /* RedLog3G10 */
    rgb.x = drt_oetf_red_log3g10(rgb.x);
    rgb.y = drt_oetf_red_log3g10(rgb.y);
    rgb.z = drt_oetf_red_log3g10(rgb.z);
  } else if (tf == 7) { /* Panasonic V-Log */
    rgb.x = drt_oetf_panasonic_vlog(rgb.x);
    rgb.y = drt_oetf_panasonic_vlog(rgb.y);
    rgb.z = drt_oetf_panasonic_vlog(rgb.z);
  } else if (tf == 8) { /* Sony S-Log3 */
    rgb.x = drt_oetf_sony_slog3(rgb.x);
    rgb.y = drt_oetf_sony_slog3(rgb.y);
    rgb.z = drt_oetf_sony_slog3(rgb.z);
  } else if (tf == 9) { /* Fuji F-Log2 */
    rgb.x = drt_oetf_fujifilm_flog2(rgb.x);
    rgb.y = drt_oetf_fujifilm_flog2(rgb.y);
    rgb.z = drt_oetf_fujifilm_flog2(rgb.z);
  }
  return rgb;
}

/* ------------------------------------------------------- EOTF transfer functions */

__DEVICE__ float3 drt_eotf_hlg(float3 rgb, int inverse) {
  /* Apply the HLG Forward or Inverse EOTF for 1000 nits.
      ITU-R Rec BT.2100-2 https://www.itu.int/rec/R-REC-BT.2100
      ITU-R Rep BT.2390-8: https://www.itu.int/pub/R-REP-BT.2390
  */
  if (inverse == 1) {
    float Yd = 0.2627f*rgb.x + 0.6780f*rgb.y + 0.0593f*rgb.z;
    rgb = rgb*drt_spowf(Yd, (1.0f - 1.2f)/1.2f);
    rgb.x = rgb.x <= 1.0f/12.0f ? _sqrtf(3.0f*rgb.x) : 0.17883277f*_logf(12.0f*rgb.x - 0.28466892f) + 0.55991073f;
    rgb.y = rgb.y <= 1.0f/12.0f ? _sqrtf(3.0f*rgb.y) : 0.17883277f*_logf(12.0f*rgb.y - 0.28466892f) + 0.55991073f;
    rgb.z = rgb.z <= 1.0f/12.0f ? _sqrtf(3.0f*rgb.z) : 0.17883277f*_logf(12.0f*rgb.z - 0.28466892f) + 0.55991073f;
  } else {
    rgb = drt_clampminf3(rgb, 0.0f);   /* sub-black codes are black; x*x/3 would mirror them into light */
    rgb.x = rgb.x <= 0.5f ? rgb.x*rgb.x/3.0f : (_expf((rgb.x - 0.55991073f)/0.17883277f) + 0.28466892f)/12.0f;
    rgb.y = rgb.y <= 0.5f ? rgb.y*rgb.y/3.0f : (_expf((rgb.y - 0.55991073f)/0.17883277f) + 0.28466892f)/12.0f;
    rgb.z = rgb.z <= 0.5f ? rgb.z*rgb.z/3.0f : (_expf((rgb.z - 0.55991073f)/0.17883277f) + 0.28466892f)/12.0f;
    float Ys = 0.2627f*rgb.x + 0.6780f*rgb.y + 0.0593f*rgb.z;
    rgb = rgb*drt_spowf(Ys, 1.2f - 1.0f);
  }
  return rgb;
}

__DEVICE__ float3 drt_eotf_pq(float3 rgb, int inverse) {
  /* Apply the ST-2084 PQ Forward or Inverse EOTF
      ITU-R Rec BT.2100-2 https://www.itu.int/rec/R-REC-BT.2100
      ITU-R Rep BT.2390-9 https://www.itu.int/pub/R-REP-BT.2390
      Note: in the spec there is a normalization for peak display luminance.
      For this function we assume the input is already normalized such that 1.0 = 10,000 nits
  */
  float m1 = 2610.0f/16384.0f;
  float m2 = 2523.0f/32.0f;
  float c1 = 107.0f/128.0f;
  float c2 = 2413.0f/128.0f;
  float c3 = 2392.0f/128.0f;

  if (inverse == 1) {
    rgb = drt_spowf3(rgb, m1);
    rgb = drt_spowf3((c1 + c2*rgb)/(1.0f + c3*rgb), m2);
  } else {
    /* decode (not in upstream's path: the DCTL only encodes). BT.2100's
       max(E'^(1/m2) - c1, 0): code 0 is 0 nits, and sub-black codes decode to 0,
       not to a negative value the signed power would carry through (code 0 came
       out at -443 nits, which turned letterbox bars into colour). */
    rgb = drt_clampminf3(drt_spowf3(rgb, 1.0f/m2) - c1, 0.0f);
    rgb = drt_spowf3(rgb/(c2 - c3*(rgb + c1)), 1.0f/m1);
  }
  return rgb;
}

/* --------------------------------------------------------- OpenDRT functions */

__DEVICE__ float drt_compress_hyperbolic_power(float x, float s, float p) {
  /* Simple hyperbolic compression function https://www.desmos.com/calculator/ofwtcmzc3w */
  return drt_spowf(x/(x + s), p);
}

__DEVICE__ float drt_compress_toe_quadratic(float x, float toe, int inv) {
  /* Quadratic toe compress function https://www.desmos.com/calculator/skk8ahmnws */
  if (toe == 0.0f) return x;
  if (inv == 0) {
    return drt_spowf(x, 2.0f)/(x + toe);
  } else {
    return (x + _sqrtf(x*(4.0f*toe + x)))/2.0f;
  }
}

__DEVICE__ float drt_compress_toe_cubic(float x, float m, float w, int inv) {
  /* https://www.desmos.com/calculator/ubgteikoke */
  if (m == 1.0f) return x;
  float x2 = x*x;
  if (inv == 0) {
    return x*(x2 + m*w)/(x2 + w);
  } else {
    float p0 = x2 - 3.0f*m*w;
    float p1 = 2.0f*x2 + 27.0f*w - 9.0f*m*w;
    float p2 = _powf(_sqrtf(x2*p1*p1 - 4.0f*p0*p0*p0)/2.0f + x*p1/2.0f, 1.0f/3.0f);
    return p0/(3.0f*p2) + p2/3.0f + x/3.0f;
  }
}

__DEVICE__ float drt_contrast_high(float x, float p, float pv, float pv_lx, int inv) {
  /* High exposure adjustment with linear extension
     https://www.desmos.com/calculator/etjgwyrgad */
  float x0 = 0.18f*_powf(2.0f, pv);
  if (x < x0 || p == 1.0f) return x;

  float o = x0 - x0/p;
  float s0 = _powf(x0, 1.0f - p)/p;
  float x1 = x0*_powf(2.0f, pv_lx);
  float k1 = p*s0*_powf(x1, p)/x1;
  float y1 = s0*_powf(x1, p) + o;
  if (inv == 1)
    return x > y1 ? (x - y1)/k1 + x1 : _powf((x - o)/s0, 1.0f/p);
  else
    return x > x1 ? k1*(x - x1) + y1 : s0*_powf(x, p) + o;
}

__DEVICE__ float drt_softplus(float x, float s) {
  /* Softplus unconstrained
     https://www.desmos.com/calculator/mr9rmujsmn */
  if (x > 10.0f*s || s < 1e-4f) return x;
  return s*_logf(_fmaxf(0.0f, 1.0f + _expf(x/s)));
}

__DEVICE__ float drt_gauss_window(float x, float w) {
  /* Simple gaussian window https://www.desmos.com/calculator/vhr9hstlyk */
  return _expf(-x*x/w);
}

__DEVICE__ float2 drt_opponent(float3 rgb) {
  /* Simple Cyan-Yellow / Green-Magenta opponent space for calculating smooth achromatic distance and hue angles */
  return make_float2(rgb.x - rgb.z, rgb.y - (rgb.x + rgb.z)/2.0f);
}

__DEVICE__ float drt_hue_offset(float h, float o) {
  /* Offset hue maintaining 0-2*pi range with modulo */
  return _fmod(h - o + DRT_PI, 2.0f*DRT_PI) - DRT_PI;
}

__DEVICE__ float3 drt_display_gamut_whitepoint(float3 rgb, float tsn, float cwp_lm, int display_gamut, int cwp) {
  /* Do final display gamut and creative whitepoint conversion. */

  /* First, convert from P3D65 to XYZ D65 */
  rgb = drt_vdot(drt_matrix_p3d65_to_xyz, rgb);

  /* Store "neutral" axis for mixing with Creative White Range control */
  float3 cwp_neutral = rgb;

  float cwp_f = _powf(tsn, 2.0f*cwp_lm);

  if (display_gamut < 3 || display_gamut >= 6) { /* D65 aligned P3 or Rec.709 display gamuts, or a linear hand-off gamut */
    if (cwp == 0) rgb = drt_vdot(drt_matrix_cat_d65_to_d93, rgb); /* D93 */
    else if (cwp == 1) rgb = drt_vdot(drt_matrix_cat_d65_to_d75, rgb); /* D75 */
    /* cwp == 2: D65, no-op */
    else if (cwp == 3) rgb = drt_vdot(drt_matrix_cat_d65_to_d60, rgb); /* D60 */
    else if (cwp == 4) rgb = drt_vdot(drt_matrix_cat_d65_to_d55, rgb); /* D55 */
    else if (cwp == 5) rgb = drt_vdot(drt_matrix_cat_d65_to_d50, rgb); /* D50 */
  }
  else if (display_gamut == 3) { /* P3-D60 */
    if (cwp == 0) rgb = drt_vdot(drt_matrix_cat_d60_to_d93, rgb); /* D93 */
    else if (cwp == 1) rgb = drt_vdot(drt_matrix_cat_d60_to_d75, rgb); /* D75 */
    else if (cwp == 2) rgb = drt_vdot(drt_matrix_cat_d60_to_d65, rgb); /* D65 */
    /* cwp == 3: D60, no-op */
    else if (cwp == 4) rgb = drt_vdot(drt_matrix_cat_d60_to_d55, rgb); /* D55 */
    else if (cwp == 5) rgb = drt_vdot(drt_matrix_cat_d60_to_d50, rgb); /* D50 */
  }
  else { /* DCI P3 or DCI X'Y'Z' */
    /* Keep "Neutral" axis as D65, don't want green midtones in P3-DCI container. */
    cwp_neutral = drt_vdot(drt_matrix_cat_dci_to_d65, rgb);
    if (cwp == 0) rgb = drt_vdot(drt_matrix_cat_dci_to_d93, rgb); /* D93 */
    else if (cwp == 1) rgb = drt_vdot(drt_matrix_cat_dci_to_d75, rgb); /* D75 */
    else if (cwp == 2) rgb = cwp_neutral;
    else if (cwp == 3) rgb = drt_vdot(drt_matrix_cat_dci_to_d60, rgb); /* D60 */
    else if (cwp == 4) rgb = drt_vdot(drt_matrix_cat_dci_to_d55, rgb); /* D55 */
    else if (cwp == 5) rgb = drt_vdot(drt_matrix_cat_dci_to_d50, rgb); /* D50 */
  }

  /* Mix between Creative Whitepoint and "neutral" axis with Creative White Range control. */
  rgb = rgb*cwp_f + cwp_neutral*(1.0f - cwp_f);

  /* RGB is now aligned to the selected creative white
     and we can convert back to the final target display gamut */
  if (display_gamut == 0) { /* Rec.709 */
    rgb = drt_vdot(drt_matrix_xyz_to_rec709, rgb);
  }
  else if (display_gamut == 5) { /* DCDM X'Y'Z' */
    /* Convert whitepoint from D65 to DCI */
    rgb = drt_vdot(drt_matrix_cat_d65_to_dci, rgb);
  }
  else { /* For all others, convert to P3D65 */
    rgb = drt_vdot(drt_matrix_xyz_to_p3d65, rgb);
  }

  /* Post creative whitepoint normalization so that peak luminance does not exceed display maximum.
     Pre-calculated constants (upstream) rather than pushing 1,1,1 through the CAT per pixel. */
  float cwp_norm = 1.0f;
  if (display_gamut == 0) { /* Rec.709 */
    if (cwp == 0) cwp_norm = 0.744192699063f; /* D93 */
    else if (cwp == 1) cwp_norm = 0.873470832146f; /* D75 */
    else if (cwp == 3) cwp_norm = 0.955936992163f; /* D60 */
    else if (cwp == 4) cwp_norm = 0.905671332781f; /* D55 */
    else if (cwp == 5) cwp_norm = 0.850004385027f; /* D50 */
  }
  else if (display_gamut == 1 || display_gamut == 2 || display_gamut >= 6) { /* P3D65, P3 Limited Rec.2020, or a hand-off */
    if (cwp == 0) cwp_norm = 0.762687057298f; /* D93 */
    else if (cwp == 1) cwp_norm = 0.884054083328f; /* D75 */
    else if (cwp == 3) cwp_norm = 0.964320186739f; /* D60 */
    else if (cwp == 4) cwp_norm = 0.923076518860f; /* D55 */
    else if (cwp == 5) cwp_norm = 0.876572837784f; /* D50 */
  }
  else if (display_gamut == 3) { /* P3D60 */
    if (cwp == 0) cwp_norm = 0.704956321013f; /* D93 */
    else if (cwp == 1) cwp_norm = 0.816715709816f; /* D75 */
    else if (cwp == 2) cwp_norm = 0.923382193663f; /* D65 */
    else if (cwp == 4) cwp_norm = 0.956138500287f; /* D55 */
    else if (cwp == 5) cwp_norm = 0.906801453023f; /* D50 */
  }
  else if (display_gamut == 4) { /* P3DCI */
    if (cwp == 0) cwp_norm = 0.665336141225f; /* D93 */
    else if (cwp == 1) cwp_norm = 0.770397131382f; /* D75 */
    else if (cwp == 2) cwp_norm = 0.870572343302f; /* D65 */
    else if (cwp == 3) cwp_norm = 0.891354547503f; /* D60 */
    else if (cwp == 4) cwp_norm = 0.855327825187f; /* D55 */
    else if (cwp == 5) cwp_norm = 0.814566436117f; /* D50 */
  }
  else if (display_gamut == 5) { /* DCDM X'Y'Z' */
    if (cwp == 0) cwp_norm = 0.707142784007f; /* D93 */
    else if (cwp == 1) cwp_norm = 0.815561082617f; /* D75 */
    else if (cwp >= 2) cwp_norm = 0.916555279740f; /* 48/52.37 for D65 and warmer (see DCI spec) */
  }

  /* only normalize values affected by range control */
  rgb = rgb*(cwp_norm*cwp_f + 1.0f - cwp_f);

  return rgb;
}

/* --------------------------------------------------------------- derive */
/* Fill the derived half of the struct from the user half. Run once per parameter
   change on the host; the result is what gets uploaded. The expressions and their
   order are the upstream DCTL's, so the floats are bit-identical to per-pixel
   evaluation. Takes and returns the struct by value: every dialect can do that. */
__DEVICE__ DrtParams drt_derive(DrtParams p) {
  /* Input gamut -> XYZ D65 */
  drt_mat3 m = drt_gamut_to_xyz(p.in_gamut);
  p.in_m00 = m.x.x; p.in_m01 = m.x.y; p.in_m02 = m.x.z;
  p.in_m10 = m.y.x; p.in_m11 = m.y.y; p.in_m12 = m.y.z;
  p.in_m20 = m.z.x; p.in_m21 = m.z.y; p.in_m22 = m.z.z;

  /* XYZ D65 -> working gamut (Input effect) */
  drt_mat3 w = drt_mat3_inverse(drt_gamut_to_xyz(p.working_gamut));
  p.wk_m00 = w.x.x; p.wk_m01 = w.x.y; p.wk_m02 = w.x.z;
  p.wk_m10 = w.y.x; p.wk_m11 = w.y.y; p.wk_m12 = w.y.z;
  p.wk_m20 = w.z.x; p.wk_m21 = w.z.y; p.wk_m22 = w.z.z;

  /* Tonescale constraint calculations https://www.desmos.com/calculator/1c4fhzy3bw */
  float ts_x1 = _powf(2.0f, 6.0f*p.tn_sh + 4.0f);
  float ts_y1 = p.tn_Lp/100.0f;
  p.ts_x0 = 0.18f + p.tn_off;
  float ts_y0 = p.tn_Lg/100.0f*(1.0f + p.tn_gb*_log2f(ts_y1));
  float ts_s0 = drt_compress_toe_quadratic(ts_y0, p.tn_toe, 1);
  p.ts_p = p.tn_con/(1.0f + float(p.tn_su)*0.05f); /* unconstrained surround compensation */
  float ts_s10 = p.ts_x0*(_powf(ts_s0, -1.0f/p.tn_con) - 1.0f);
  float ts_m1 = ts_y1/_powf(ts_x1/(ts_x1 + ts_s10), p.tn_con);
  p.ts_m2 = drt_compress_toe_quadratic(ts_m1, p.tn_toe, 1);
  p.ts_s = p.ts_x0*(_powf(ts_s0/p.ts_m2, -1.0f/p.tn_con) - 1.0f);
  p.ts_dsc = p.eotf == 4 ? 0.01f : (p.eotf == 5 ? 0.1f : 100.0f/p.tn_Lp);

  /* Lerp from pt_cmp at 100 nits to pt_cmp_hdr at 1000 nits */
  float pt_cmp_Lf = p.pt_hdr*_fminf(1.0f, (p.tn_Lp - 100.0f)/900.0f);
  /* Approximate scene-linear scale at Lp=100 nits */
  p.s_Lp100 = p.ts_x0*(_powf((p.tn_Lg/100.0f), -1.0f/p.tn_con) - 1.0f);
  p.ts_s1 = p.ts_s*pt_cmp_Lf + p.s_Lp100*(1.0f - pt_cmp_Lf);

  /* Contrast Low constants (upstream computes these inside the enable branch) */
  p.lcon_m = _powf(2.0f, -p.tn_lcon);
  float lcon_w = p.tn_lcon_w/4.0f;
  lcon_w *= lcon_w;
  p.lcon_w = lcon_w;
  /* Normalize for ts_x0 intersection constraint: https://www.desmos.com/calculator/blyvi8t2b2 */
  p.lcon_cnst_sc = drt_compress_toe_cubic(p.ts_x0, p.lcon_m, p.lcon_w, 1)/p.ts_x0;

  /* Contrast High */
  p.hcon_p = _powf(2.0f, p.tn_hcon);

  /* Rendering space weights */
  p.rs_wr = p.rs_rw;
  p.rs_wg = 1.0f - p.rs_rw - p.rs_bw;
  p.rs_wb = p.rs_bw;

  return p;
}

/* ------------------------------------------------------- render (linear) */
/* Everything between "linear P3-D65 in" and "display-linear out": the rendering
   space, tonescale, hue and purity modules, creative white, display gamut. This is
   the upstream transform() body minus the input decode in front and the clamp +
   EOTF behind, split out so the inverse below can iterate on it. Output is
   display-linear in the forward's own convention (SDR 1.0 = display peak; PQ and
   HLG scaled by ts_dsc as upstream does before encoding). */
/* XYZ (D65 white) -> a display gamut (DRT_DG_*), the containers with their own
   white adapted so a neutral stays neutral; the P3-D60 container uses the P3-D65
   primaries as the rendering does. For the Un-tone-mapped view only. */
__DEVICE__ float3 drt_xyz_to_display_gamut(DRT_PARAMS_ARG p, float3 xyz, int dg) {
  if (dg == 0) return drt_vdot(drt_matrix_xyz_to_rec709, xyz);
  if (dg == 1) return drt_vdot(drt_matrix_xyz_to_p3d65, xyz);
  if (dg == 2) return drt_vdot(drt_mat3_inverse(drt_matrix_rec2020_to_xyz), xyz);
  if (dg == 3) return drt_vdot(drt_matrix_xyz_to_p3d65, drt_vdot(drt_matrix_cat_d65_to_d60, xyz));
  if (dg == 4) return drt_vdot(drt_mat3_inverse(drt_matrix_p3dci_to_xyz), drt_vdot(drt_matrix_cat_d65_to_dci, xyz));
  if (dg == 5) return drt_vdot(drt_matrix_cat_d65_to_dci, xyz);
  if (dg == 7) return drt_vdot(drt_mat3_inverse(drt_matrix_ap0_to_xyz), xyz);   /* ACES 2065-1 (the CAT is in the matrix) */
  if (dg == 8) return drt_vdot(drt_mat3_inverse(drt_matrix_ap1_to_xyz), xyz);   /* ACEScg */
  if (dg == 9) return drt_vdot(drt_mat3_inverse(drt_matrix_rec2020_to_xyz), xyz);   /* Rec.2020, plain */
  return drt_vdot(drt_make_mat3(   /* 6: the working gamut */
    make_float3(p.wk_m00, p.wk_m01, p.wk_m02),
    make_float3(p.wk_m10, p.wk_m11, p.wk_m12),
    make_float3(p.wk_m20, p.wk_m21, p.wk_m22)), xyz);
}

__DEVICE__ float3 drt_render_linear(DRT_PARAMS_ARG p, float3 rgb) {

  /* Rendering Space: "Desaturate" to control scale of the color volume in the rgb ratios.
     Controlled by rs_sa (saturation) and red and blue weights (rs_rw and rs_bw) */
  float3 rs_w = make_float3(p.rs_wr, p.rs_wg, p.rs_wb);
  float sat_L = rgb.x*rs_w.x + rgb.y*rs_w.y + rgb.z*rs_w.z;
  rgb = sat_L*p.rs_sa + rgb*(1.0f - p.rs_sa);

  /* Offset */
  rgb += p.tn_off;

  /* Tonescale Norm */
  float tsn = drt_hypotf3(rgb)/DRT_SQRT3;

  /* RGB Ratios */
  rgb = drt_sdivf3f(rgb, tsn);

  float2 opp = drt_opponent(rgb);
  float ach_d = drt_hypotf2(opp)/2.0f;

  /* Smooth ach_d, normalized so 1.0 doesn't change https://www.desmos.com/calculator/ozjg09hzef */
  ach_d = (1.25f)*drt_compress_toe_quadratic(ach_d, 0.25f, 0);

  /* Hue angle, rotated so that red = 0.0 */
  float hue = _fmod(_atan2f(opp.x, opp.y) + DRT_PI + 1.10714931f, 2.0f*DRT_PI);

  /* RGB Hue Angles
     Wider than CMY by default. R towards M, G towards Y, B towards C */
  float3 ha_rgb = make_float3(
    drt_gauss_window(drt_hue_offset(hue, 0.1f), 0.66f),
    drt_gauss_window(drt_hue_offset(hue, 4.3f), 0.66f),
    drt_gauss_window(drt_hue_offset(hue, 2.3f), 0.66f));

  /* RGB Hue Angles for hue shift: red shifted more orange */
  float3 ha_rgb_hs = make_float3(
    drt_gauss_window(drt_hue_offset(hue, -0.4f), 0.66f),
    ha_rgb.y,
    drt_gauss_window(drt_hue_offset(hue, 2.5f), 0.66f));

  /* CMY Hue Angles
     Exact alignment to Cyan/Magenta/Yellow secondaries would be PI, PI/3 and -PI/3, but
     we customize these a bit for creative purposes: M towards B, Y towards G, C towards G */
  float3 ha_cmy = make_float3(
    drt_gauss_window(drt_hue_offset(hue, 3.3f), 0.5f),
    drt_gauss_window(drt_hue_offset(hue, 1.3f), 0.5f),
    drt_gauss_window(drt_hue_offset(hue, -1.15f), 0.5f));

  /* Brilliance */
  if (p.brl_enable != 0) {
    float brl_tsf = _powf(tsn/(tsn + 1.0f), 1.0f - p.brl_rng);
    float brl_exf = (p.brl + p.brl_r*ha_rgb.x + p.brl_g*ha_rgb.y + p.brl_b*ha_rgb.z)*_powf(ach_d, 1.0f/p.brl_st);
    float brl_ex = _powf(2.0f, brl_exf*(brl_exf < 0.0f ? brl_tsf : 1.0f - brl_tsf));
    tsn *= brl_ex;
  }

  /* Contrast Low */
  if (p.tn_lcon_enable != 0) {
    tsn *= p.lcon_cnst_sc;
    tsn = drt_compress_toe_cubic(tsn, p.lcon_m, p.lcon_w, 0);
  }

  /* Contrast High */
  if (p.tn_hcon_enable != 0) {
    tsn = drt_contrast_high(tsn, p.hcon_p, p.tn_hcon_pv, p.tn_hcon_st, 0);
  }

  /* Hyperbolic Compression */
  float tsn_pt = drt_compress_hyperbolic_power(tsn, p.ts_s1, p.ts_p);
  float tsn_const = drt_compress_hyperbolic_power(tsn, p.s_Lp100, p.ts_p);
  tsn = drt_compress_hyperbolic_power(tsn, p.ts_s, p.ts_p);

  /* Hue Contrast R */
  if (p.hc_enable != 0) {
    float hc_ts = 1.0f - tsn_const;
    /* Limit high purity on bottom end and low purity on top end by ach_d.
       This helps reduce artifacts and over-saturation. */
    float hc_c = hc_ts*(1.0f - ach_d) + ach_d*(1.0f - hc_ts);
    hc_c *= ach_d*ha_rgb.x;
    hc_ts = _powf(hc_ts, 1.0f/p.hc_r_rng);
    /* Bias contrast based on tonescale using Lift/Mult: https://www.desmos.com/calculator/gzbgov62hl */
    float hc_f = p.hc_r*(hc_c - 2.0f*hc_c*hc_ts) + 1.0f;
    rgb = make_float3(rgb.x, rgb.y*hc_f, rgb.z*hc_f);
  }

  /* Hue Shift RGB by purity compress tonescale, shifting more as intensity increases */
  if (p.hs_rgb_enable != 0) {
    float3 hs_rgb = make_float3(
      ha_rgb_hs.x*ach_d*_powf(tsn_pt, 1.0f/p.hs_r_rng),
      ha_rgb_hs.y*ach_d*_powf(tsn_pt, 1.0f/p.hs_g_rng),
      ha_rgb_hs.z*ach_d*_powf(tsn_pt, 1.0f/p.hs_b_rng));
    float3 hsf = make_float3(hs_rgb.x*p.hs_r, hs_rgb.y*-p.hs_g, hs_rgb.z*-p.hs_b);
    hsf = make_float3(hsf.z - hsf.y, hsf.x - hsf.z, hsf.y - hsf.x);
    rgb += hsf;
  }

  /* Hue Shift CMY by tonescale, shifting less as intensity increases */
  if (p.hs_cmy_enable != 0) {
    float tsn_pt_compl = 1.0f - tsn_pt;
    float3 hs_cmy = make_float3(
      ha_cmy.x*ach_d*_powf(tsn_pt_compl, 1.0f/p.hs_c_rng),
      ha_cmy.y*ach_d*_powf(tsn_pt_compl, 1.0f/p.hs_m_rng),
      ha_cmy.z*ach_d*_powf(tsn_pt_compl, 1.0f/p.hs_y_rng));
    float3 hsf = make_float3(hs_cmy.x*-p.hs_c, hs_cmy.y*p.hs_m, hs_cmy.z*p.hs_y);
    hsf = make_float3(hsf.z - hsf.y, hsf.x - hsf.z, hsf.y - hsf.x);
    rgb += hsf;
  }

  /* Purity Compression https://www.desmos.com/calculator/adtzkjofgn */
  /* Purity Limit Low */
  float pt_lml_p = 1.0f + 4.0f*(1.0f - tsn_pt)*(p.pt_lml + p.pt_lml_r*ha_rgb_hs.x + p.pt_lml_g*ha_rgb_hs.y + p.pt_lml_b*ha_rgb_hs.z);
  float ptf = 1.0f - _powf(tsn_pt, pt_lml_p);

  /* Purity Limit High */
  float pt_lmh_p = (1.0f - ach_d*(p.pt_lmh_r*ha_rgb_hs.x + p.pt_lmh_b*ha_rgb_hs.z))*(1.0f - p.pt_lmh*ach_d);
  ptf = _powf(ptf, pt_lmh_p);

  /* Mid-Range Purity: boosts mid-range purity on the low end and reduces it on the high end */
  if (p.ptm_enable != 0) {
    float ptm_low_f;
    if (p.ptm_low_st == 0.0f || p.ptm_low_rng == 0.0f) ptm_low_f = 1.0f;
    else ptm_low_f = 1.0f + p.ptm_low*_expf(-2.0f*ach_d*ach_d/p.ptm_low_st)*_powf(1.0f - tsn_const, 1.0f/p.ptm_low_rng);
    float ptm_high_f;
    if (p.ptm_high_st == 0.0f || p.ptm_high_rng == 0.0f) ptm_high_f = 1.0f;
    else ptm_high_f = 1.0f + p.ptm_high*_expf(-2.0f*ach_d*ach_d/p.ptm_high_st)*_powf(tsn_pt, 1.0f/(4.0f*p.ptm_high_rng));
    ptf *= ptm_low_f*ptm_high_f;
  }

  /* Lerp to peak achromatic by ptf in rgb ratios */
  rgb = rgb*ptf + 1.0f - ptf;

  /* Inverse Rendering Space */
  sat_L = rgb.x*rs_w.x + rgb.y*rs_w.y + rgb.z*rs_w.z;
  rgb = (sat_L*p.rs_sa - rgb)/(p.rs_sa - 1.0f);

  /* Convert to final display gamut and set whitepoint */
  rgb = drt_display_gamut_whitepoint(rgb, tsn_const, p.cwp_lm, p.display_gamut, p.cwp);

  /* Post Brilliance */
  if (p.brlp_enable != 0) {
    float2 brlp_opp = drt_opponent(rgb);
    float brlp_ach_d = drt_hypotf2(brlp_opp)/4.0f;
    brlp_ach_d = 1.1f*(brlp_ach_d*brlp_ach_d/(brlp_ach_d + 0.1f));
    float3 brlp_ha_rgb = ach_d*ha_rgb;
    float brlp_m = p.brlp + p.brlp_r*brlp_ha_rgb.x + p.brlp_g*brlp_ha_rgb.y + p.brlp_b*brlp_ha_rgb.z;
    float brlp_ex = _powf(2.0f, brlp_m*brlp_ach_d*tsn);
    rgb *= brlp_ex;
  }

  /* Purity Compress Low */
  if (p.ptl_enable != 0) rgb = make_float3(drt_softplus(rgb.x, p.ptl_c), drt_softplus(rgb.y, p.ptl_m), drt_softplus(rgb.z, p.ptl_y));

  /* Final tonescale adjustments */
  tsn *= p.ts_m2; /* scale for inverse toe */
  tsn = drt_compress_toe_quadratic(tsn, p.tn_toe, 0);
  tsn *= p.ts_dsc; /* scale for display encoding */

  /* Return from RGB ratios */
  rgb *= tsn;

  /* Rec.2020 (P3 Limited) */
  if (p.display_gamut == 2) {
    rgb = drt_clampminf3(rgb, 0.0f); /* Limit to P3 gamut */
    rgb = drt_vdot(drt_matrix_p3_to_rec2020, rgb);
  }
  /* None: the rendered P3-D65 light back into a linear hand-off gamut */
  else if (p.display_gamut >= 6) {
    rgb = drt_xyz_to_display_gamut(p, drt_vdot(drt_matrix_p3d65_to_xyz, rgb), p.display_gamut);
  }

  return rgb;
}

/* Display-linear -> encoded by a DRT_EOTF_* (clamp first when asked, as upstream). */
__DEVICE__ float3 drt_encode_eotf(float3 rgb, int eotf, int clamp) {
  if (clamp != 0) rgb = drt_clampf3(rgb, 0.0f, 1.0f);
  float eotf_p = 2.0f + float(eotf)*0.2f;
  if ((eotf > 0) && (eotf < 4)) rgb = drt_spowf3(rgb, 1.0f/eotf_p);
  else if (eotf == 4) rgb = drt_eotf_pq(rgb, 1);
  else if (eotf == 5) rgb = drt_eotf_hlg(rgb, 1);
  return rgb;
}
/* Display-linear -> encoded, as upstream: clamp, then inverse EOTF. */
__DEVICE__ float3 drt_encode(DRT_PARAMS_ARG p, float3 rgb) { return drt_encode_eotf(rgb, p.eotf, p.clamp_out); }

/* Encoded -> display-linear: the inverse of drt_encode. Sub-black codes are
   black (the signed power would carry them through as negative light). */
__DEVICE__ float3 drt_decode(DRT_PARAMS_ARG p, float3 rgb) {
  rgb = drt_clampminf3(rgb, 0.0f);
  float eotf_p = 2.0f + float(p.eotf)*0.2f;
  if ((p.eotf > 0) && (p.eotf < 4)) rgb = drt_spowf3(rgb, eotf_p);
  else if (p.eotf == 4) rgb = drt_eotf_pq(rgb, 0);
  else if (p.eotf == 5) rgb = drt_eotf_hlg(rgb, 0);
  return rgb;
}

/* --------------------------------------------------------------- inverse */
/* NOT IN UPSTREAM. Display-encoded pixels (per p.display_gamut / p.eotf, with
   p's look) back to linear P3-D65 scene values, so a finished delivery can enter
   a scene-referred comp and round-trip through drt_transform() unchanged. The
   equivalent of an ACES "Output - Rec.709" inverse ODT.
     1. decode the EOTF;
     2. exact neutral inverse of the tonescale as the starting point (the DCTL
        already carries inverse forms of the toe, cubic and contrast-high pieces;
        the hyperbolic compression inverts in closed form);
     3. Newton iterations on drt_render_linear() with a finite-difference Jacobian
        for the chroma modules, which have no closed inverse.
   Round-trips to ~1e-4 relative per channel (under 0.2 of an 8-bit step, PQ's foot
   included) for anything inside the display gamut and below the clip; clipped or
   out-of-gamut input has no scene value and gets a best effort. */
__DEVICE__ float drt_compress_hyperbolic_power_inv(float y, float s, float p) {
  /* y = (x/(x+s))^p  ->  x = s*y^(1/p)/(1 - y^(1/p)) */
  if (y <= 0.0f) return y;
  float u = _powf(_fminf(y, 0.999f), 1.0f/p);
  return s*u/(1.0f - u);
}

__DEVICE__ float drt_tonescale_neutral_inv(DRT_PARAMS_ARG p, float t) {
  /* Inverse of the achromatic path: display-linear (before clamp) -> scene norm */
  t = t/p.ts_dsc;
  t = drt_compress_toe_quadratic(t, p.tn_toe, 1);
  t = t/p.ts_m2;
  t = drt_compress_hyperbolic_power_inv(t, p.ts_s, p.ts_p);
  if (p.tn_hcon_enable != 0) t = drt_contrast_high(t, p.hcon_p, p.tn_hcon_pv, p.tn_hcon_st, 1);
  if (p.tn_lcon_enable != 0) t = drt_compress_toe_cubic(t, p.lcon_m, p.lcon_w, 1)/p.lcon_cnst_sc;
  return t;
}

__DEVICE__ float3 drt_abs3(float3 a) { return make_float3(_fabs(a.x), _fabs(a.y), _fabs(a.z)); }

struct drt_inv_result {
  float3 s;
  float err;
};

/* Residual weights: each channel's error is measured against that channel's own
   level (plus 1 % of the pixel's max as a floor), not in absolute display-linear.
   PQ's foot spends an 8-bit code step per decade below 0.01 nits, so a channel
   that sits near black (a saturated colour in a P3 container) needs an absolute
   accuracy a thousand times finer than its bright neighbours; an absolute
   tolerance that is fine for the bright channels leaves it steps off, and an
   unweighted sum of squares stops improving it once the bright channels are at
   float resolution. */
#define DRT_INV_TOL 1e-8f   /* sum of squared relative residuals: ~1e-4 per channel */
__DEVICE__ float3 drt_inverse_weights(float3 d, float m) {
  float fl = 0.01f*m;
  return make_float3(1.0f/(_fabs(d.x) + fl), 1.0f/(_fabs(d.y) + fl), 1.0f/(_fabs(d.z) + fl));
}

/* The prior. The forward is many-to-one for bright saturated colour (purity
   compression pulls everything bright toward white), so with a channel in the
   top fifth of the range a display value has a whole family of sources, members
   hundreds apart with channels of either sign, and an exact solve picks one by
   accident: neighbouring pixels land in different members and any grade draws a
   contour between them (measured: steps of 10 to 60 code values between inputs
   two code values apart, none below 80 % of the peak). The fixed-point warm
   start is a continuous map of the pixel, so inside that band the solve is
   pulled toward it, with a weight that rises from nothing at the band's foot to
   dominant at the cap, and the result blended into it the same way; the rim of a
   clipped highlight then comes back bounded and continuous, a little less
   saturated than the master had it. The band hangs off the Highlight Cap, so
   with the cap off (1.0, the default) nothing changes below the peak. */
#define DRT_INV_MU 0.0f      /* flat prior, everywhere: none */
#define DRT_INV_BAND 0.8f    /* the band starts at this fraction of the cap */
#define DRT_INV_PULL 10.0f   /* prior curvature at the cap, as a multiple of the data's */

/* Objective: weighted residual squared plus the prior. */
struct drt_inv_cost { float3 f; float e; };
__DEVICE__ drt_inv_cost drt_inverse_cost(DRT_PARAMS_ARG p, float3 s, float3 d, float3 w, float3 s0, float mu) {
  drt_inv_cost c;
  c.f = (drt_render_linear(p, s) - d)*w;
  float3 q = s - s0;
  c.e = c.f.x*c.f.x + c.f.y*c.f.y + c.f.z*c.f.z + mu*(q.x*q.x + q.y*q.y + q.z*q.z);
  return c;
}

/* One damped multiplicative step toward d: each channel scaled by (d/H)^0.7,
   held within a factor of four. Monotone in intensity and continuous in d. */
__DEVICE__ float drt_inverse_ratio(float d, float h) {
  if (d <= 0.0f || h <= 1e-8f) return 1.0f;
  float r = d/h;
  r = r < 0.25f ? 0.25f : (r > 4.0f ? 4.0f : r);
  return _powf(r, 0.7f);
}

/* One solve from the starting guess s0: fixed-point warm start, then Levenberg-Marquardt. */
__DEVICE__ drt_inv_result drt_inverse_solve(DRT_PARAMS_ARG p, float3 d, float m, float3 s0, float t, float kb) {
  /* Refine. First the fixed point (cheap, monotone in intensity, pulls the chroma
     guess the right way, and a continuous map of d: it runs the same way for
     neighbouring pixels, where a choice between several starts would not), then
     Levenberg-Marquardt on W*(H(s) - d) = 0 plus the prior toward the warm start,
     with a finite-difference Jacobian, W the per-channel weights above. LM rather
     than plain Newton because the toe and the shoulder flatten the map and the
     chroma modules bend it, so the Jacobian is often ill-conditioned; damping
     keeps every step a descent step. A step is kept only if it lowers the
     objective. */
  float lo = -1.0e4f;                 /* P3 components of wide-gamut scene colours go negative; the forward copes */
  float hi = 1.0e4f;
  float3 w = drt_inverse_weights(d, m);
  float it = 1.0f/_fmaxf(t, 1e-4f);   /* the prior's scale: 1/intensity */
  float3 s = s0;
  float tol = DRT_INV_TOL;

  for (int k = 0; k < 8; k++) {
    float3 hv = drt_render_linear(p, s);
    float3 r = make_float3(drt_inverse_ratio(d.x, hv.x), drt_inverse_ratio(d.y, hv.y), drt_inverse_ratio(d.z, hv.z));
    if (_fabs(r.x - 1.0f) < 1e-3f && _fabs(r.y - 1.0f) < 1e-3f && _fabs(r.z - 1.0f) < 1e-3f) break;
    s = make_float3(drt_clampf((s.x + p.tn_off)*r.x - p.tn_off, lo, hi),
                    drt_clampf((s.y + p.tn_off)*r.y - p.tn_off, lo, hi),
                    drt_clampf((s.z + p.tn_off)*r.z - p.tn_off, lo, hi));
  }
  s0 = s;   /* the prior pulls toward the warm start */
  float mu = DRT_INV_MU*it*it;   /* the prior's curvature, per channel; the band adds to it below */
  drt_inv_cost c0 = drt_inverse_cost(p, s, d, w, s0, mu);
  float3 f0 = c0.f;
  float err = c0.e;

  float lambda = 1e-3f;
#ifndef DRT_INV_LM_ITERS
#define DRT_INV_LM_ITERS 24
#endif
  for (int k = 0; k < DRT_INV_LM_ITERS; k++) {
    if (err < tol) break;
    float3 h = make_float3(_fmaxf(1e-4f, 1e-3f*_fabs(s.x)), _fmaxf(1e-4f, 1e-3f*_fabs(s.y)), _fmaxf(1e-4f, 1e-3f*_fabs(s.z)));
    float3 fx = (drt_render_linear(p, make_float3(s.x + h.x, s.y, s.z)) - d)*w;
    float3 fy = (drt_render_linear(p, make_float3(s.x, s.y + h.y, s.z)) - d)*w;
    float3 fz = (drt_render_linear(p, make_float3(s.x, s.y, s.z + h.z)) - d)*w;
    /* Jacobian columns */
    float3 jx = (fx - f0)/h.x;
    float3 jy = (fy - f0)/h.y;
    float3 jz = (fz - f0)/h.z;
    /* normal equations A = J^T J + mu I, g = J^T f0 + mu (s - s0); in the band the
       prior's curvature follows the data's, so the pull is the same at every level */
    float axx = jx.x*jx.x + jx.y*jx.y + jx.z*jx.z;
    float ayy = jy.x*jy.x + jy.y*jy.y + jy.z*jy.z;
    float azz = jz.x*jz.x + jz.y*jz.y + jz.z*jz.z;
    if (kb > 0.0f) {
      float band = kb*kb*DRT_INV_PULL*(axx + ayy + azz)/3.0f;
      if (band > mu) {
        mu = band;
        c0 = drt_inverse_cost(p, s, d, w, s0, mu); f0 = c0.f; err = c0.e;
      }
    }
    axx += mu; ayy += mu; azz += mu;
    float axy = jx.x*jy.x + jx.y*jy.y + jx.z*jy.z;
    float axz = jx.x*jz.x + jx.y*jz.y + jx.z*jz.z;
    float ayz = jy.x*jz.x + jy.y*jz.y + jy.z*jz.z;
    float gx = jx.x*f0.x + jx.y*f0.y + jx.z*f0.z + mu*(s.x - s0.x);
    float gy = jy.x*f0.x + jy.y*f0.y + jy.z*f0.z + mu*(s.y - s0.y);
    float gz = jz.x*f0.x + jz.y*f0.y + jz.z*f0.z + mu*(s.z - s0.z);
    int accepted = 0;
    for (int j = 0; j < 8; j++) {
      /* (A + lambda*diag(A)) delta = -g, Cramer on the symmetric 3x3 */
      float m00 = axx*(1.0f + lambda), m11 = ayy*(1.0f + lambda), m22 = azz*(1.0f + lambda);
      float det = m00*(m11*m22 - ayz*ayz) - axy*(axy*m22 - ayz*axz) + axz*(axy*ayz - m11*axz);
      if (_fabs(det) < 1e-30f) { lambda *= 10.0f; continue; }
      float bx = -gx, by = -gy, bz = -gz;
      float dx = (bx*(m11*m22 - ayz*ayz) - axy*(by*m22 - ayz*bz) + axz*(by*ayz - m11*bz))/det;
      float dy = (m00*(by*m22 - ayz*bz) - bx*(axy*m22 - ayz*axz) + axz*(axy*bz - by*axz))/det;
      float dz = (m00*(m11*bz - by*ayz) - axy*(axy*bz - by*axz) + bx*(axy*ayz - m11*axz))/det;
      if (!(dx == dx) || !(dy == dy) || !(dz == dz)) { lambda *= 10.0f; continue; }
      float3 cand = make_float3(drt_clampf(s.x + dx, lo, hi), drt_clampf(s.y + dy, lo, hi), drt_clampf(s.z + dz, lo, hi));
      drt_inv_cost cc = drt_inverse_cost(p, cand, d, w, s0, mu);
      if (cc.e < err) { s = cand; f0 = cc.f; err = cc.e; lambda = _fmaxf(lambda*0.1f, 1e-6f); accepted = 1; break; }
      lambda *= 10.0f;
    }
    if (accepted == 0) break;
  }
  drt_inv_result r;
  r.s = s + (s0 - s)*kb;   /* and blend into the warm start across the band */
  r.err = err;
  return r;
}

__DEVICE__ float3 drt_inverse_transform(DRT_PARAMS_ARG p, float3 enc) {
  /* 1. to display-linear, held under the Highlight Cap when one is set: a fraction
        of the peak luminance (Lp in display-linear is 1.0 for SDR, Lp/10000 for PQ,
        Lp/1000 for HLG), per channel, as a display clips. */
  float3 d = drt_decode(p, enc);
  if (p.inv_cap > 0.0f && p.inv_cap < 1.0f) {
    float capL = p.inv_cap*p.tn_Lp*p.ts_dsc/100.0f;
    d = make_float3(_fminf(d.x, capL), _fminf(d.y, capL), _fminf(d.z, capL));
  }

  /* 2. starting guess: neutral tonescale inverse on the max, display ratios as chroma */
  float m = _fmaxf(drt_fmaxf3_(d), 1e-6f);
  float t = drt_tonescale_neutral_inv(p, m);
  float3 s0 = d*(t/m) - p.tn_off;

  /* how far into the band under the cap the brightest channel sits (0 below it, 1
     at the cap). With the cap off the band sits above the peak instead, where
     nothing round-trips anyway, so a master brighter than Peak Luminance still
     comes back bounded. */
  float peak = p.tn_Lp*p.ts_dsc/100.0f;
  float u = (p.inv_cap > 0.0f && p.inv_cap < 1.0f)
          ? (m/(p.inv_cap*peak) - DRT_INV_BAND)/(1.0f - DRT_INV_BAND)
          : (m/peak - 1.0f)*10.0f;
  u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
  float k = u*u*(3.0f - 2.0f*u);

  /* 3. one solve from it, no second opinions: a choice between basins is a step in the image */
  return drt_inverse_solve(p, d, m, s0, t, k).s;   /* linear P3-D65 */
}

/* ------------------------------------------------------- input transform */
/* The media-side half (the "minColor Input" effect): linearise by p.in_oetf,
   take p.in_gamut to XYZ D65, land in the linear working gamut p.working_gamut.
   With an inverse in_oetf entry (>= DRT_OETF_INVERSE_FIRST) the pixels are a
   delivery made by this look through the display encoding the entry names (the
   host has already put that encoding into display_gamut / eotf / tn_su):
   drt_inverse_transform() takes them back to linear P3-D65; in_gamut is ignored.
   `p` must be derived. */
__DEVICE__ float3 drt_input_transform(DRT_PARAMS_ARG p, float3 rgb) {
  /* limited (video) range left unexpanded by the host: 64..940 of 1023 -> 0..1,
     the same on all three channels (this is RGB after the host's matrix) */
  if (p.in_range == 1) rgb = (rgb - 64.0f/1023.0f)*(1023.0f/876.0f);
  drt_mat3 xyz_to_wk = drt_make_mat3(
    make_float3(p.wk_m00, p.wk_m01, p.wk_m02),
    make_float3(p.wk_m10, p.wk_m11, p.wk_m12),
    make_float3(p.wk_m20, p.wk_m21, p.wk_m22));
  if (p.in_oetf >= 16) { /* DRT_OETF_INVERSE_FIRST: any "OpenDRT inverse: <encoding>" entry */
    rgb = drt_inverse_transform(p, rgb);
    rgb = drt_vdot(drt_matrix_p3d65_to_xyz, rgb);
    rgb = drt_vdot(xyz_to_wk, rgb);
    return rgb;
  }
  rgb = drt_linearize(rgb, p.in_oetf);
  drt_mat3 in_to_xyz = drt_make_mat3(
    make_float3(p.in_m00, p.in_m01, p.in_m02),
    make_float3(p.in_m10, p.in_m11, p.in_m12),
    make_float3(p.in_m20, p.in_m21, p.in_m22));
  rgb = drt_vdot(in_to_xyz, rgb);
  rgb = drt_vdot(xyz_to_wk, rgb);
  return rgb;
}

/* ------------------------------------------------------------ macOS fix */
/* NOT IN UPSTREAM. The "minColor macOS Fix" effect: the macOS display of
   ocio/mincolor-unmanaged.ocio as an effect with no settings, for Adobe-engine projects. sRGB Display codes
   (2.2 power, Rec.709) re-encoded for After Effects' Display P3 viewport on macOS:
   decode 2.2, Rec.709 -> P3-D65 primaries, encode 2.2. A workaround for how AE shows
   colour on macOS, not a creative choice; viewer only (put it on a Guide Layer). */
__DEVICE__ float3 drt_macos_fix(float3 rgb) {
  rgb = drt_spowf3(drt_clampminf3(rgb, 0.0f), 2.2f);
  rgb = drt_vdot(drt_matrix_xyz_to_p3d65, drt_vdot(drt_matrix_rec709_to_xyz, rgb));
  return drt_spowf3(rgb, 1.0f/2.2f);
}

/* ------------------------------------------------------------- transform */
/* One pixel. `p` must have been through drt_derive(). rgb is in the input
   encoding selected by p.in_gamut / p.in_oetf; the result is display-encoded
   per p.display_gamut / p.eotf (0..1 for SDR, PQ or HLG signal for HDR).
   Two views, as an OCIO display has: p.out_view 0 renders the scene through
   OpenDRT; 1, "Un-tone-mapped", is the Input's conversion run backwards, the
   gamut matrix and the display curve and nothing else (no tonescale, no
   purity, no clip; scene 1.0 is display white on a power curve and 100 nits
   on PQ and HLG, as OCIO's view puts it), so a picture that came in through
   the Input leaves as itself. */
__DEVICE__ float3 drt_transform(DRT_PARAMS_ARG p, float3 rgb) {

  /* Linearize if a non-linear input oetf / transfer function is selected */
  rgb = drt_linearize(rgb, p.in_oetf);

  /* Convert from input gamut into XYZ */
  drt_mat3 in_to_xyz = drt_make_mat3(
    make_float3(p.in_m00, p.in_m01, p.in_m02),
    make_float3(p.in_m10, p.in_m11, p.in_m12),
    make_float3(p.in_m20, p.in_m21, p.in_m22));
  rgb = drt_vdot(in_to_xyz, rgb);

  if (p.out_view == 1) {
    rgb = drt_xyz_to_display_gamut(p, rgb, p.display_gamut);
    float s = p.eotf == 4 ? 0.01f : (p.eotf == 5 ? 0.1f : 1.0f);   /* 1.0 = 100 nits on PQ (1.0 = 10000) and HLG (1.0 = 1000) */
    return drt_encode_eotf(rgb*s, p.eotf, 0);
  }

  rgb = drt_vdot(drt_matrix_xyz_to_p3d65, rgb);
  rgb = drt_render_linear(p, rgb);
  return drt_encode(p, rgb);
}

#endif /* OPENDRT_KERNEL_H */
