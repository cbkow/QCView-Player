---
title: HDR
permalink: /hdr/
nav_order: 15
---

# SDR vs HDR

The **Display** list at the bottom right of the Color panel switches between the SDR and HDR modes your system offers. Modes the current display can't show are greyed out.

For the most part, you will want `SDR — sRGB` on both macOS and Windows, `HDR10 PQ` for HDR on Windows, and `EDR — Linear P3` for HDR on macOS.

The presets list reacts to the mode you are in: presets that don't suit it are dimmed, and each preset is tagged **SDR** or **HDR**.

![Display mode list at the bottom of the Color panel](images/qcv032.jpg)

---

## Color output with OCIO

### SDR mode

In SDR mode, use an `sRGB` or a `Rec.1886` output for a standard display-referred result.

To check an HDR master on an SDR display, turn on the clip's [Highlight Knee](/color/#highlight-knee): it compresses the highlights above your chosen source peak into the display's range instead of clipping them, and shows an amber **KNEE** pill while it does.

---

### HDR mode (Windows)

With Windows `HDR10 PQ` mode enabled, use `Rec.2100-PQ` or a similarly named PQ output.

---

### HDR mode (macOS — EDR)

In `EDR — Linear P3` mode, use the matching `Linear P3 EDR` output in the ACES 2.0 or Blender 5.2 configs (`Linear sRGB EDR` for `EDR — Linear sRGB`).

---

## HDR sources

Pick the source's colorspace as the clip's **Input**: `Rec.2100-PQ` for a BT.2020 PQ master, `ST2084-P3-D65` for a PQ master graded in P3-D65 (Resolve's "P3-D65 ST2084"), `Rec.2100-HLG` for HLG. The Input stays with that clip, so SDR and HDR clips can sit side by side in the same project or dual view.

The Inspector's **HDR metadata** row shows the file's MaxCLL, MaxFALL and mastering peak when the container carries them.
