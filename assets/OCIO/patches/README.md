# OCIO config patches

QCView ships Blender's stock OCIO configs with a small EDR patch on top
(2 linear-light display colorspaces + views for macOS EDR output — see
docs/hdr.md), plus one input colorspace Blender lacks: `ST2084-P3-D65`
(PQ with P3-D65 primaries — the common Resolve HDR deliverable; aliased
`ST2084-P3-D65 - Display` so presets resolve identically in the ACES 2.0
config, which ships it stock). These .patch files record that delta so
the next Blender config upgrade is mechanical:

```
cp -R /Applications/Blender.app/Contents/Resources/<ver>/datafiles/colormanagement assets/OCIO/Blender<ver>
cd assets/OCIO/Blender<ver>
patch config.ocio < ../patches/blender52-edr.patch
```

If the hunks drift on a future config, the invariants that must hold:
the EDR display colorspaces land values where 1.0 = 100-nit SDR white
(encoding: display-linear, from_display_reference via
cie_xyz_d65_interchange → Linear Rec.709 / Linear DCI-P3 D65), and the
new displays/views must be appended to the explicit active_displays /
active_views lists or OCIO hides them.

The ACES 2.0 config carries the same patch shape (display names suffixed
" - Display", views reuse ASWF's ACES 2.0 HDR transforms); no stock copy
of its base (ASWF studio-config-all-views v4.0.0) is kept locally, so
regenerate its diff from the upstream download if it ever needs a rebase.

## Names QCView depends on

The bundled configs change with every Blender / ASWF release, and QCView
refers to parts of them by name. After replacing any config in
`assets/OCIO/`, walk this list — code sites carry a `CONFIG UPGRADE`
comment (`git grep "CONFIG UPGRADE"`).

1. **Run the probe.** `build/tools/probe-ocio-metal/probe-ocio-metal
   assets/OCIO` — the `names` lines check every scope tag rule
   (`src/color/scope_names.h`) against the bundled configs; a `FAIL` on
   Blender 5.2 must be fixed (add an alias in the config patch, or the new
   name to the list). `--` lines on other configs are informational: the
   scopes read that file kind through the built-in config instead. The
   rest of the probe (OCIO chain, knee, viewer aids, scopes on the GPU vs
   the CPU) also runs against Blender 5.2 and ACES 2.0 by name.
2. **Interchange roles.** The scopes and the knee split need
   `aces_interchange` (scene side) and `cie_xyz_d65_interchange` (display
   side) — `OcioChainBuilder::buildScopeTransform` / `buildSplit`.
3. **Default and fallback config.** `Blender5.2` is the default
   (`ocio_config_manager.cpp`, friendly-name table + promotion) and the
   scopes' fallback for files the live config can't name
   (`kScopeFallbackConfigDir` in `src/window/scope_controller.cpp`, shown
   as "built-in config" on the scope badge). A new default directory moves
   all three.
4. **Built-in presets** (`src/color/preset_manager.cpp`) name config,
   input, display and view by string — load each once.
5. **Capture chain** (`OCIOConfigManager::sdrCaptureDisplayView`) looks for
   the `sRGB` / `sRGB - Display` display.
6. **Scale heuristic.** `ScopeController`'s `scaleFor` falls back to
   colourspace-name substrings (PQ / HLG / 2100 / ST2084 / Linear / Log /
   ACES …) when a colourspace has no `encoding`; recheck against new names.
7. **This patch.** The EDR displays and `ST2084-P3-D65` (above) are ours,
   not Blender's — re-apply and re-verify.
8. **Badge short names** (`src/color/colourspace_short_name.h`): the
   project panel and A/B chips shorten a clip's Input with generic word
   rules (drop " - Display" / " - Texture" / "Encoded", Linear → Lin, Wide
   Gamut → WG, "to ACES2065-1" → "→ AP0", cut near 20 characters).
   `build/tools/probe-ocio-pins/probe-ocio-pins <config.ocio>` checks the
   rules and that no two colourspaces share a short name — run it on each
   new config.

