/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 * Derived from OpenDRT v1.1.0 by Jed Smith (https://github.com/jedypod/open-display-transform), GPLv3;
 * modified 2026-09-30 to 2026-10-04, see CHANGES-FROM-OPENDRT.md. Not affiliated with or endorsed by OpenDRT.
 */
/* DRT core — HLSL (Shader Model 5.0+) shim.
 *
 * STATUS: NOT YET COMPILE-CHECKED. No dxc/fxc on the machine this was written on.
 * The first D3D11 host (a host's Windows renderer, or the AE DirectX kernel) must
 * run it through D3DCompile / dxc and fix what it finds; the rules in
 * opendrt_kernel.h were written with HLSL in mind, so expect little.
 *
 * Host source:
 *   #include "opendrt_shim_hlsl.h"
 *   #include "opendrt_params.h"
 *   #include "opendrt_kernel.h"
 *   cbuffer DrtCb : register(b0) { DrtParams drtParams; };
 *   ... drt_transform(drtParams, rgb)
 * DrtParams is passed by value (DRT_PARAMS_ARG below); 104 scalars, which the
 * compiler flattens. HLSL fmod has C semantics; exp10 does not exist, so it is
 * pow(10, x).
 */

#define __DEVICE__
#define __CONSTANT__ static const

#define make_float2(a, b)    float2((a), (b))
#define make_float3(a, b, c) float3((a), (b), (c))

#define _powf(a, b)   pow((a), (b))
#define _expf(a)      exp((a))
#define _exp2f(a)     exp2((a))
#define _exp10f(a)    pow(10.0f, (a))
#define _logf(a)      log((a))
#define _log2f(a)     log2((a))
#define _sqrtf(a)     sqrt((a))
#define _fabs(a)      abs((a))
#define _fmod(a, b)   fmod((a), (b))
#define _floorf(a)    floor((a))
#define _atan2f(a, b) atan2((a), (b))
#define _fminf(a, b)  min((a), (b))
#define _fmaxf(a, b)  max((a), (b))

#define DRT_PARAMS_ARG DrtParams
#define DRT_AGX_ARG DrtAgxParams
