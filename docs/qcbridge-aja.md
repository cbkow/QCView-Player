---
title: AJA
parent: QCBridge Add-Ons
permalink: /qcbridge/aja/
nav_order: 4
---

# QCBridgeA for AJA SDI and HDMI

[QCBridgeA](https://github.com/cbkow/QCBridgeA) sends what QCView shows and plays out of an **AJA device's SDI and HDMI outputs** as a video signal: 10-bit RGB at 1080p or UHD, at the source's frame rate, SDR, PQ or HLG, with the mixed audio embedded. It is the way to put QCView's viewport on a reference monitor.

Like QCBridgeN it runs as its own small application beside QCView. QCView starts it when the output is switched on; it takes the device exclusively, the way AJA's own applications do, and releases it when the output is switched off or QCView quits.

---

## Requirements

- **QCView 3.0** on the same machine, with **QCBridgeA 0.0.2 or later**. 0.0.2 queues frames for the card, which makes playback smoother.
- An AJA device with playback and **AJA's own software installed** for the driver (AJA Desktop Software 17.x). UHD goes out as one 12G stream and needs a device with 12G routing.
- Windows 10 1809 or later, x64, or macOS 13 or later on Apple Silicon. With QCView 3.0 it has been run on one device: a **T-TAP Pro on macOS**, at 1080p and UHD, in SDR, PQ and HLG. The Windows build has not yet been run on a card with QCView 3.0.

---

## Installation

Install AJA's software for the device first. Then download the installer from the [QCBridgeA releases](https://github.com/cbkow/QCBridgeA/releases):

- Windows: `QCBridgeA-<version>-Setup-x64.exe`
- macOS: `QCBridgeA-<version>-arm64.pkg`

Each installs the application and registers it with QCView, which lists it in the Output section from the next launch. Updating is the same step with a newer installer. The Windows installer is not code-signed yet, so SmartScreen asks once.

---

## Setup

1. In QCView, open the **Output** section in the left rail. The **AJA** card shows the sink, with its settings on the card.
2. Choose the **transfer**: SDR Rec.709, PQ Rec.2100 or HLG Rec.2100. For PQ, choose the mastering primaries (Rec.2020 or P3-D65) and the peak, MaxCLL and MaxFALL in nits the monitor should read.
3. Choose the **raster**: **Auto** sends UHD for anything above 1080p and 1080p otherwise, switching when the source changes; **1080p** and **UHD** fix it.
4. Switch the card on. Its dot turns green once the sink is attached and the output is live, and its lines show what QCView sends (raster, format, transfer) and the sink's report: the device, the format and the connectors, for example "T-TAP Pro UHDp25 10-bit PQ 2020, SDI + HDMI RGB". Hover the dot when it is not green: it says whether QCView is waiting for the sink, or why the output was refused.

<!-- SHOT qcv3_053: the AJA card switched on and live - green dot, the two status lines naming the raster / format / transfer and the device, with Transfer on PQ Rec.2100 and the Primaries and Peak / MaxCLL / MaxFALL fields -->

The transfer and the raster can be changed while it runs; the card reconfigures in place, a brief interruption and no stop and start. If another application holds the device, the card says which one.

---

## Picture and colour

The picture is the viewport's: the clip's colour chain, dual-view layouts and the difference view go out as on screen, letterboxed into the raster. The card's **Transfer** is the encoding of the wire. QCView encodes the picture for it — SDR, PQ in the primaries you chose, or HLG — and the sink signals the same in the SDI VPID and the HDMI InfoFrame, with the mastering metadata for PQ. See [HDR](/hdr/) for the colour side.

Other things to know:

- The frame rate follows the source: 23.976, 24, 25, 29.97, 30, 50, 59.94 and 60 progressive. A rate change mid-session reconfigures the card.
- SDI carries YCbCr 4:2:2 from the card's colour-space converter (Rec.709 for SDR, Rec.2020 for PQ and HLG). HDMI carries RGB 4:4:4 full range, except UHD above 30p, where HDMI 2.0 has no room for 10-bit RGB and the HDMI side carries YCbCr; the card says so.
- The audio is the mixed audio QCView plays, embedded as 48 kHz PCM, stereo in channels 1 and 2. **Mute feed**, the speaker in the Output section's header, silences it — and it starts muted, so switch it on to hear sound on the monitor.
- Guides and note strokes go out as layers when their toggles in the Output section's header are on. Note strokes go out only while the Notes panel is open in single view.

---

## Why a separate application

The sink talks to the AJA driver through AJA's NTV2 SDK and holds the device for the session; keeping that in its own process keeps QCView's own renderer and its licence simple, and a sink that fails leaves QCView running. QCView hands it frames through shared memory, the same bus QCBridgeN uses. The design and the measurements are in the project's `PLAN.md` and `lab/`.

AJA, KONA, Io and T-TAP are trademarks of AJA Video Systems, Inc.; QCBridgeA is not an AJA product. The AJA device driver is not part of QCBridgeA; install it from aja.com.
