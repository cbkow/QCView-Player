/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 */
/* minColor AgX, host side (C++ only; mincolor_agx.cpp). Included by opendrt.h. */
#pragma once

namespace drt {

/* Blender's AgX: ACEScg working gamut, target Rec.709, peak 100, range -10 / +6.5 EV,
   contrast 2.4, toe and shoulder 1.5, hue restore 0.6, HDR purity 0.5, outset 1. */
DrtAgxParams drt_agx_defaults();

/* Fill the derived matrices and curve constants from the user fields. */
DrtAgxParams drt_agx_derive(DrtAgxParams a);

} // namespace drt
