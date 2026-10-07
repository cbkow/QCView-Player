# Vendored from minColorAE

The minColor engine's maths: copied unmodified from
`github.com/cbkow/minColorAE` `core/` at commit **e824933** (0.1.3, 2026-10-05),
GPL-3.0-only, © 2026 cbkow (the same author and licence as QCView). The rendering
is derived from OpenDRT v1.1.0 by Jed Smith (GPL-3.0), modified in minColorAE —
see that repository's `CHANGES-FROM-OPENDRT.md`; the AgX port's provenance is in its
`CHANGES-AGX.md`. Not affiliated with or endorsed by OpenDRT, darktable or Blender.

Keep in sync by re-copying, never by editing here. `opendrt_grade.h` is vendored
only because `opendrt.h` (the aggregate the .cpp files include) and the preset
tables reference it; QCView has no Grade and never calls it. Not vendored: the
CUDA / GLSL shims and the presets JSON (the tables in `opendrt_presets.cpp` are
the same data, compiled in).

Files: opendrt.h, opendrt_params.h, opendrt_kernel.h, opendrt_grade.h,
opendrt_presets.{h,cpp}, opendrt_shim_{cpp,msl,hlsl}.h, mincolor_knee.h,
mincolor_agx.{h,cpp}, mincolor_agx_host.h.
