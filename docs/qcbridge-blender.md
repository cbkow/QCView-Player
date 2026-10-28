---
title: Blender
parent: QCBridge Add-Ons
permalink: /qcbridge/blender/
nav_order: 1
---

# QCBridge for Blender

![QCBridge](/images/qcbridge_001.jpg)

**QCBridge for Blender's** primary purpose is to sync a live Blender project to Blender on another computer and use that second computer to generate a live Eevee or Cycles preview - a way to keep your main working computer’s resources dedicated to live operations and use a second for live-preview rendering. It works over VPNs; there is the option to use QCview as the live preview window.

It has two roles — **Host** (the machine you work on) and **Replica** (the render box).

---

## Requirements

- **Blender 4.5 or newer** on both machines (5.2 LTS is the tested target).
- Both machines on the same network, or connected by a VPN (WireGuard-class works well).
- The project files reachable from both machines — typically shared network storage.
- For streaming into QCView: **QCView 2.2.4+** (or higher) on both machines.

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

![Agent settings window](/images/qcbridge_015.png)

### Shared storage

- **Shared folder** — the folder on shared storage that both machines see. If you are sharing between multiple operating systems, ensure that you path map that network share below too.

![Agent settings window](/images/qcbridge_013.png)

- **Path mapping** — one row per storage root the two machines share, in both spellings, so file paths translate between platforms. Use this for any shared storage (Network volumes, LucidLink paths, etc.) so a receiver on another platform can load the same project.
 
![Agent settings window](/images/qcbridge_013.png)

Windows note: the stream arrives over UDP. If QCView can't connect to the replica, allow the stream port through the replica's firewall (default 9998):

```
netsh advfirewall firewall add rule name="QCBridge SRT" dir=in action=allow protocol=UDP localport=9998
```

---

## Using it

![QCBridge](/images/qcbridge_014.png)

Start Blender on both machines and press **Start Session** in the *QC Bridge* panel (sidebar of the 3D viewport, `N` key). If the tray apps are connected this will launch a new blender instance, load the project remotely and start rendering it live. (Depending on the project size and computer speed, this will take a moment or two.)

![QCBridge](/images/qcbridge_005.png)

From the host panel:

- **Stream to QCView here** (preference, on by default) — QCView on this machine picks the stream up by itself: the add-on writes it a card when the session starts and QCView listens for the replica. Open QCView whenever you like; the **Open in QCView** button re-writes the card (it launches nothing). With the preference off, the button opens the legacy `qcview://stream` link and the replica listens instead.
- **Shot Mode** — locks the replica to the camera frame: fitted to the stream, matted in black, holding steady while you orbit your scene freely. The timeline still follows. This framing matches your render output.
- **Replica Zoom −/+/reset** — punches the replica's camera view in or out without changing your own viewport.
- **Pause Sync** — holds your edits back (queued, not lost) while you try something messy; resume flushes them.
- **Force Resync** — resyncs the whole file if something is not updating naturally.

In QCView the stream is a live media item: load it on one side of a Dual View against an approved render.

**How QCView joins (QCBridge 0.4 and later).** QCView never gets launched by Blender. When a session starts, the host add-on writes a small card into QCView's Inputs folder on the host machine (`~/Library/Application Support/QCView/Inputs/blender.json` on macOS, `%ProgramData%\QCView\Inputs\blender.json` on Windows): the port, the passphrase, the colour setup, the mismatch rows. QCView watches that folder (**Settings ▸ Blender sessions**, on by default), opens an SRT listener on the card's port as a live clip, and the replica's encoder calls in. The card is rewritten when the colour setup changes and removed when the session stops; the clip then shows "Blender session ended". **File ▸ Connect to Blender** picks a card up by hand. The QCView port is a host-panel preference (9998 by default); the passphrase derives from the pairing token, as before.

**Colour (QCBridge 0.3 and later).** The stream is scene-referred: the replica's viewport leaves Blender as linear light, PQ-encoded with scene 1.0 at 100 nits in a BT.2020 container, so the view transform is applied in QCView, exactly as Blender would. The card (or the legacy link) pins the clip's **Input** to decode the wire: `Rec.2100-PQ Scene` on the OCIO engine, `Rec.2020 · PQ 100` on minColor. It is a normal clip pin, visible in the Color panel and yours to change, and a re-written card never overwrites a pin already there. Nothing is read from the stream itself: the card names the wire, the working space it was converted from (the file's Working Space in Blender 5.2, so a Linear Rec.2020 file arrives as Linear Rec.2020), the host's OCIO config, display device, view transform and look, and only a wire QCView knows by name is accepted. If colour management is off, the card turns it on and says so in a toast; it never turns it off.

**The host's view, for the host's display.** On the OCIO engine with the Blender config, the link pins a **Host display** on the clip: the host scene's view transform (AgX, Standard, Filmic, whatever the host uses — nothing is assumed), rendered for the host's **Display Device**, then carried to the Display you drive. That is the picture the artist sees, including the muted picture a Rec.2020 or Display P3 device gives in Blender's sRGB window (Blender 5.2 writes those codes as they are; QCView does the same, "as shown"). The live strip shows it as a chip, `Host display Rec.2020 · AgX`, with a menu to switch the emulation off, to convert display to display instead ("true emulation", for a host that really emulates), or to use your own View. The Color panel shows the same line with ↺. A view the Blender config does not have on that display is refused by name and said so; a display the config lacks is matched to the nearest bundled one by its primaries and named. White balance, curves and the HDR view are not reproduced; the chip's red line says which. Your **Output** is never touched: the output surface is this machine's.

With the minColor engine on screen only the Input is pinned; minColor knows AgX alone, so the host's view is not reproduced and a toast says so. The host's display and view only have to exist in your active config — a studio config that carries them is as good as the bundled one; when they do not resolve the toast offers the switch to Blender 5.2. If you add the stream by URL instead, pick the Input yourself and the view transform you use in Blender. Older QCBridge (0.2) streams are display-referred sRGB: leave the Input at the default.

The stream's colour tags, when the encoder writes them, reach the scopes with the session, so a PQ stream measures in nits without further setup. If the encoder leaves them out, the **Transfer** chip on the live strip names the encoding; see [HDR sources without tags](/hdr/#hdr-sources-without-tags).
