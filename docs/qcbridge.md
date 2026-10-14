---
title: QCBridge Add-Ons
permalink: /qcbridge/
nav_order: 18
has_children: true
has_toc: false
---

# QCBridge Add-Ons

The QCBridge add-ons connect QCView to other software and hardware: two bring live pictures **in**, two send the viewport **out**.

**In:**

- **The [QCBridgeAE for After Effects and Premiere Pro](/qcbridge/adobe/)** is a Mercury Transmit plugin that streams live viewport output from the the AE or Premiere to QCView. The output is unaltered and full float, converted to 1/2 float in QCView so the image quality is preserved under all circumstances, including HDR scenarios.
- **[QCBridge for Blender](/qcbridge/blender/)** is a bit different. It’s primary purpose is to sync a live Blender project to Blender on another computer and use that second computer to generate a live Eevee or Cycles preview - a way to keep your main working computer’s resources dedicated to live operations and use a second for live-preview rendering. It works over VPNs; there is the option to use QCview as the live preview window.

**Out** — both drive the [Output](/output/) section, which sends the viewport with its colour transform, guides and notes, plus the mixed audio:

- **[QCBridgeN for NDI®](/qcbridge/ndi/)** offers the viewport on the local network as an NDI source named QCView, for any NDI receiver.
- **[QCBridgeA for AJA SDI and HDMI](/qcbridge/aja/)** sends it out of an AJA device as a 10-bit signal at 1080p or UHD, SDR, PQ or HLG, for a reference monitor.

The Output section also streams the viewport as H.264 to a review service such as Louper without an add-on. A live feed from the Adobe or Blender add-on can be sent on any of these ways.

All four are separate downloads from QCView, and all are actively in development.

