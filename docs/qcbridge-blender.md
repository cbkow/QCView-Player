---
title: Blender
parent: QCBridge Add-Ons
permalink: /qcbridge/blender/
nav_order: 1
---

# QCBridge for Blender

![QCBridge](/images/qcbridge_001.jpg)

**QCBridge for Blender's** primary purpose is to sync a live Blender project to Blender on another computer and use that second computer to generate a live Eevee or Cycles preview - a way to keep your main working computer’s resources dedicated to live operations and use a second for live-preview rendering. It works over VPNs; you can use QCview as the live preview window.

It has two roles — **Host** (the machine you work on) and **Replica** (the render box).

---

## Requirements

- **Blender 4.5 or newer** on both machines (5.2 LTS is the tested target).
- Both machines on the same network, or connected by a VPN (WireGuard-class works well).
- The project files reachable from both machines — typically shared network storage.
- For the live preview in QCView: **QCView 3.0** on the host (the machine you work on). The replica needs `ffmpeg`; QCView installed there is the easy way — its bundled ffmpeg is found on its own.

---

## Installation

QCBridge is two installs on **each machine**, both from the [QCBridge releases](https://github.com/cbkow/QCBridge/releases):

**1. The agent** — a small tray app that makes the connection between both computers and holds your settings. (Needs setup.)

- macOS: `QCBridge-Agent-<version>-arm64.pkg`
- Windows: `QCBridge-Agent-<version>-Setup-x64.exe` 

**2. The extension** — the zip goes into Blender as usual: **Edit → Preferences → Get Extensions**, the dropdown arrow (top-right), **Install from Disk…**, pick the zip.

Updating later is the same two steps with newer files. Your settings live with the agent and survive updates and reinstalls.

---

## Setup

Everything about the connection lives in the **QCBridge Agent** and is set in its settings window — the same window on both machines. Open it from the tray icon's **Settings…** item, or from **Edit → Preferences → Add-ons → QC Bridge → Open Agent Settings** in Blender. Changes apply as you make them; nothing needs a restart.

![Agent settings window](/images/qcbridge_009.png)

### This machine

![Agent settings window](/images/qcbridge_010.png)

- **Name** — shown to the other machine and in the tray. Empty means the computer's name.
- **Role** — **Send scene** on the host (your workstation); **Receive scene** on the replica (the render box). One agent per machine; the switch takes effect at once.
  

![Agent settings window](/images/qcbridge_011.png)

### Pairing

- **Token** — any short phrase, the same on both machines. 
- **Receiver** (host only) — the replica's address, or pick it from **Find receivers** and press **Pair**. **Forget** trusts it afresh.

![Agent settings window](/images/qcbridge_012.png)

### Replica Hosting

- **Blender** — if you are in `Receive scene` mode, ensure this points to your active Blender install.
- **Kiosk** — the Blender the replica opens for a session shows the viewport only, no UI. On by default.
- **Resolution** — the size of the window that Blender opens, which is the stream's resolution: **1920x1080** by default, 2560x1440, 1280x720, fullscreen (the old behaviour, the stream takes the screen's size) or any custom `WxH`. The viewport, the frame ring and the encoder all mean the same pixels, so Cycles renders no more than the stream carries. On a Retina Mac the window is opened at half the size in points, so the pixels come out right.

![Agent settings window](/images/qcbridge_015.png)

### Shared storage

- **Shared folder** — the folder on shared storage that both machines see. If you are sharing between multiple operating systems, ensure that you path map that network share below too.

![Agent settings window](/images/qcbridge_013.png)

- **Path mapping** — one row per storage root the two machines share, in both spellings, so file paths translate between platforms. Use this for any shared storage (Network volumes, LucidLink paths, etc.) so a receiver on another platform can load the same project.

![Agent settings window](/images/qcbridge_016.png)

Windows note: the stream arrives over UDP, and since QCBridge 0.4 the replica calls QCView on the host. If the panel says the replica is calling and QCView never shows a picture, allow the QCView port through the **host's** firewall (9998 by default):

```
netsh advfirewall firewall add rule name="QCView SRT" dir=in action=allow protocol=UDP localport=9998
```

With **Stream to QCView here** off (the legacy flow, where the replica listens and QCView calls) the same rule goes on the replica instead.

---

## Using it

![QCBridge](/images/qcbridge_014.png)

Start Blender on both machines and press **Start Session** in the *QC Bridge* panel (sidebar of the 3D viewport, `N` key). If the tray apps are connected this launches a new Blender on the replica at the stream's resolution, loads the project there and starts rendering it live. (Depending on the project size and computer speed, this takes a moment or two.) In QCView, toggle **File ▸ Connect to Blender** — before or after — and the picture arrives within a second or two of the replica's first frame.

![QCBridge](/images/qcbridge_005.png)

The host panel says what matters in one line — `● Live · 1920x1080 · QCView connected`, or what is still missing (the replica calling with nobody answering, a file the replica could not find) — in red when something needs you. Two folded sub-panels hold the rest: **Colour** (both ends' colour setup, side by side, with the differences that matter) and **Details** (the full readout: sync counts, bootstraps, the stream, the agent). The actions:

- **Stop Session** — ends the session on both ends; the replica's Blender quits and QCView reports "Blender session ended".
- **Shot Mode** — locks the replica to the camera frame: fitted to the stream, matted in black, holding steady while you orbit your scene freely. The timeline still follows. This framing matches your render output.
- **Replica Zoom −/+/reset** — punches the replica's camera view in or out without changing your own viewport.
- **Pause Sync** — holds your edits back (queued, not lost) while you try something messy; resume flushes them.
- **Force Resync** — resyncs the whole file if something is not updating naturally.
- **Stream to QCView here** (preference, on by default) — QCView on this machine picks the stream up when you toggle Connect to Blender. With the preference off, the panel shows the legacy **Open in QCView** button and a copy button for the `qcview://stream` link, and the replica listens instead of calling.

Switching the project on the host (opening another file) ships the new file once; the replica follows, the stream restarts if the file's working space differs, and the clip in QCView is renamed after the file.

In QCView the stream is a live media item: load it on one side of a [Dual View](/dual-view/#live-sides) against an approved render. Connect to Blender has to be on for the session's clip to listen there too.

**How QCView joins (QCBridge 0.4 and later).** QCView never gets launched by Blender. When a session starts, the host add-on writes a small card into QCView's Inputs folder on the host machine (`~/Library/Application Support/QCView/Inputs/blender.json` on macOS, `%ProgramData%\QCView\Inputs\blender.json` on Windows): the port, the passphrase, the colour setup, the mismatch rows. QCView uses the port and the passphrase, and shows the mismatch rows that are errors under the **Blender sessions** switch in Settings; it does not act on the colour setup (see Colour below). The passphrase is never shown: not in the Inspector, the recent items or the log. QCView does nothing with it until you ask: **File ▸ Connect to Blender** (the same switch as **Settings ▸ Blender sessions**, off at every launch) makes QCView watch that folder, open an SRT listener on the card's port as a live clip, and the replica's encoder calls in — now, or when the session starts. The card is rewritten when the colour setup changes and removed when the session stops; QCView then reports "Blender session ended" and keeps the clip in the bin, and a session started again reconnects on its own. Toggling Connect to Blender off closes the listener and removes the clip; deleting the clip from the bin does the same. New Project or Open Project keep the connection: the clip joins the new project, and takes the screen only when the project restored nothing else. The QCView port is a host-panel preference (9998 by default); the passphrase derives from the pairing token, as before.

**Files and libraries.** Textures and caches are never copied: the replica reads them from shared storage through the path mapping, which is why the mapping rows matter. A linked library the replica cannot place — a `.blend` on a path only the host has — is shipped once by the host (up to 64 MB) and cached on the replica under Blender's config folder, so the next session needs no resend. A library the host cannot read either is reported as missing on the host too. The panel's summary line names the files the replica could not find.

**When something is off.** Each end writes a per-stage timeline of the session — `qcbridge-timeline-host.log` and `qcbridge-timeline-replica.log` in Blender's config folder (`~/Library/Application Support/Blender/5.2/config` on macOS, `%APPDATA%\Blender Foundation\Blender\5.2\config` on Windows) — with a line per step: the handshake, every file transfer and why it was sent, the first frame, the encoder, the card, the viewer connecting. Read the two side by side before guessing.

Color (QCBridge 0.4 and later).** The stream is scene-referred: the replica's viewport leaves Blender as linear light, PQ-encoded with scene 1.0 at 100 nits in a BT.2020 container. QCView shows it on its own terms. The stream's color tags tell it what the wire is, so the **Transfer** chip on the live row reads `Auto · PQ 2020` without any setup.

**Nothing in Blender changes QCView's color.** The host's colour setup — its OCIO config, display device, view transform, look, exposure — travels in the card, but QCView 3.0 does not act on it: it does not set the clip's Input, does not switch Color on, and does not reproduce the host's view. The two are different applications showing pictures on different displays. If you want a view transform on the stream, give the clip a chain in the **Color** drawer as you would for any clip, and it is yours to change. The wire is PQ in Rec.2020, so that is the chain's Input: the Input gamut and transfer in minColor, or the matching Input colourspace in an OCIO chain (the Blender 5.2 config is bundled). See [Color](/color/).

The Color sub-panel in Blender still compares the two Blender ends, host and replica; that comparison is about them, not about QCView.

**A stream opened by hand.** With **Stream to QCView here** off, the stream is opened from the legacy link or with **File ▸ Open Stream…**. A listener address you open yourself listens for that session without the Connect to Blender switch; the passphrase goes in the dialog's own hidden field. If the encoder leaves the color tags out, pick the encoding on the live row's **Transfer** chip; see [HDR](/hdr/).
