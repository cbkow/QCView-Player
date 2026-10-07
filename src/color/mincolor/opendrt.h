/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 * Derived from OpenDRT v1.1.0 by Jed Smith (https://github.com/jedypod/open-display-transform), GPLv3;
 * modified 2026-09-30 to 2026-10-04, see CHANGES-FROM-OPENDRT.md. Not affiliated with or endorsed by OpenDRT.
 */
/* DRT core — the C++ entry point.
 *
 * Include this one header from C++ hosts (the AE effect's CPU path, a host's LUT
 * export and probes, the tools here). It wraps the shared dialect-neutral sources in
 * namespace drt so their DCTL-style names (float3, _powf, ...) never leak.
 *
 * Usage:
 *   drt::DrtParams p = drt::drt_resolve(settings);   // or fill the StickShift fields yourself
 *   p = drt::drt_derive(p);                          // drt_resolve() already did this
 *   float out[3]; drt::apply(p, in, out);
 *
 * The same DrtParams bytes are what a GPU host uploads as its uniform block.
 */
#pragma once

#include <cmath>
#include <cstddef>

namespace drt {

#include "opendrt_shim_cpp.h"
#include "opendrt_params.h"
#include "opendrt_kernel.h"
#include "opendrt_grade.h"
#include "mincolor_knee.h"
#include "mincolor_agx.h"

static_assert(sizeof(DrtParams) == DRT_PARAMS_SCALARS * 4,
              "DrtParams must stay a flat block of 4-byte scalars, count a multiple of 4 "
              "(see opendrt_params.h). Pad with the reserved_* slots.");
static_assert(sizeof(DrtGradeParams) == DRT_GRADE_SCALARS * 4,
              "DrtGradeParams must stay a flat block of 4-byte scalars, count a multiple of 4.");
static_assert(sizeof(DrtAgxParams) == DRT_AGX_SCALARS * 4 && DRT_AGX_SCALARS % 4 == 0,
              "DrtAgxParams must stay a flat block of 4-byte scalars, count a multiple of 4.");

/* One pixel through a derived parameter block. */
inline void apply(const DrtParams &p, const float in[3], float out[3])
{
    const float3 r = drt_transform(p, make_float3(in[0], in[1], in[2]));
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
}

} // namespace drt

#include "opendrt_presets.h"
#include "mincolor_agx_host.h"
