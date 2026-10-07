/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 * Derived from OpenDRT v1.1.0 by Jed Smith (https://github.com/jedypod/open-display-transform), GPLv3;
 * modified 2026-09-30 to 2026-10-04, see CHANGES-FROM-OPENDRT.md. Not affiliated with or endorsed by OpenDRT.
 */
/* DRT core — Metal Shading Language shim.
 *
 * The host source is:
 *   #include <metal_stdlib>
 *   using namespace metal;
 *   #include "opendrt_shim_msl.h"
 *   #include "opendrt_params.h"
 *   #include "opendrt_kernel.h"
 *   ... a kernel that binds `constant DrtParams& p` and calls drt_transform(p, rgb)
 * or the same four files concatenated as one string (another host builds its OCIO kernel
 * that way). Compile-checked by tools/check_msl.metal (ctest target check_msl).
 *
 * MSL has exp10 and C-semantics fmod, so nothing is emulated here.
 */

#define __DEVICE__ static inline
#define __CONSTANT__ constant

#define make_float2(a, b)    float2((a), (b))
#define make_float3(a, b, c) float3((a), (b), (c))

#define _powf(a, b)   pow((a), (b))
#define _expf(a)      exp((a))
#define _exp2f(a)     exp2((a))
#define _exp10f(a)    exp10((a))
#define _logf(a)      log((a))
#define _log2f(a)     log2((a))
#define _sqrtf(a)     sqrt((a))
#define _fabs(a)      fabs((a))
#define _fmod(a, b)   fmod((a), (b))
#define _floorf(a)    floor((a))
#define _atan2f(a, b) atan2((a), (b))
#define _fminf(a, b)  fmin((a), (b))
#define _fmaxf(a, b)  fmax((a), (b))

#define DRT_PARAMS_ARG constant DrtParams &
#define DRT_AGX_ARG constant DrtAgxParams &
