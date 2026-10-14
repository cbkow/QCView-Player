---
title: Installation
permalink: /installation/
nav_order: 2
---

# Installation

## Windows

Download QCView from the [Microsoft Store](https://apps.microsoft.com/detail/9p4z15p5g805) for automatic updates:

<a href="https://apps.microsoft.com/detail/9p4z15p5g805?referrer=appbadge&mode=full" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200" style="border: 1px solid #707070; border-radius: 4.5px;"/>
</a>

### Requirements

- Windows 10 version 1809 or later (10.0.17763.0), x64.
- Vulkan-capable GPU with up-to-date drivers (FFmpeg software fallback when unavailable).
- HDR-capable display + Windows HDR mode enabled for HDR10 output.

---

## macOS

Download the latest release [here](https://github.com/cbkow/QCView-Player/releases/latest/).

<a href="https://github.com/cbkow/QCView-Player/releases/latest/download/QCView-MacOS.dmg" target="_blank" rel="noopener noreferrer">
  <img src="https://qcview.app/images/download.png" width="200"/>
</a>

1. Open the `.dmg` file.
2. Drag **QCView** to your **Applications** folder.
3. Launch from Applications or Spotlight.

### Updates

Once installed in **Applications**, QCView keeps itself up to date — it checks once a day and offers to download and install new versions in place. You can also check any time from **About → Check for Updates…**, or turn automatic checks off in **Settings → Updates**. (In-place updates require running QCView from the Applications folder, not from the mounted disk image.)

### Requirements

- macOS 13.0 (Ventura) or later.
- Apple Silicon (arm64) — native Metal rendering, no Rosetta.
- EDR-capable display recommended for HDR workflows.

### Add-ons

See the [QCViewBridge](/qcbridge/) page for installation and usage guides for the Adobe After Effects, Adobe Premiere Pro, and Blender add-ons. They have their own separate installers.

---

## Version History

What's new in 2.5.7

- **AJA SDI / HDMI output** — with [QCBridgeA](https://github.com/cbkow/QCBridgeA/releases) installed, the Output section drives an AJA device (tested on a T-TAP Pro): the viewport and the mixed audio go out over SDI and HDMI as 10-bit RGB, at 1080p, 720p or UHD, at the source's frame rate. See [Output](/output/).
- **Transfer per output** — each output that carries it has a transfer: SDR Rec.709, PQ Rec.2100 or HLG Rec.2100. It is what the wire is told and signals; the picture goes out as the viewport shows it, so the display is chosen once in the colour panel (OCIO or minColor) and a PQ or HLG feed from Premiere Pro or After Effects passes straight through. PQ adds the mastering metadata a monitor reads: Rec.2020 or P3-D65 primaries, peak, MaxCLL and MaxFALL. Changing the transfer re-tags the running output without a stop and start.
- **Raster per output** — for a sink that leaves it to QCView: Auto sends UHD (3840×2160) for anything above 1080p and 1080p otherwise, switching when the source changes; 1080p and UHD fix it.
- **Output section** — one card per output and for the stream: the name, one word of state and Start / Stop in the header, a summary line while the card is closed, the controls behind a chevron. The feed mute and the guides and notes layers are a row of chips at the top.
- **Windows** — File ▸ Exit (Ctrl+Q); macOS keeps Quit in the application menu.
- **Fixes** — two QML binding loops in the Inspector (the card title and the chip-row label) no longer log on every launch.

What's new in 2.5.6

- **Stream output** — the Output section can send the viewport and the mixed audio to a review service such as Louper, or to any SRT listener or RTMP server, as H.264 + AAC encoded inside QCView by the hardware encoder (VideoToolbox on macOS, Media Foundation on Windows), with no OBS in between. URL and stream key, video bitrate, resolution, frame rate, Baseline or Main profile, audio bitrate and SRT latency, live counters while it runs, and a session that reconnects on its own if the connection drops. See [Output](/output/).
- **Output section** — Output moves out of Settings into its own collapsible section in the left rail, and the sink rows get a Start / Stop button like the stream's.
- **Notes on live feeds** — a note on a Premiere Pro, After Effects or SRT feed freezes the feed and captures the frame. Notes live in the project's `.qcview/streams/` folder under the item's name, stamped with the capture time and the host's timecode; an unsaved project shows a Save tile first. Space freezes the feed and brings it back, Escape returns to live, clicking a note shows its frame again for review, ← / → walk the notes, and the outputs show what the viewport shows. A LIVE / REVIEW pill in the Notes panel and the live strip say which you are looking at. Exports head stream notes with their capture time, and **Open in minNotes** hands the notes folder to minNotes 1.0.3.
- **Live audio** — Premiere Pro's audio no longer chops: the jitter buffer fills to its target before it plays and fills again after an underrun.
- **Live feeds and the transport** — the transport keys no longer pause or mute a live feed; M still mutes it. In dual view with a live side, stepping and shuttling while paused no longer silence the live side.
- **Projects** — Save As to another folder carries a stream's notes with it; the live strip gains a Notes panel button.

What's new in 2.5.5

- **Output on Windows** — the NDI feed from the D3D11 renderer no longer skipped every other frame, and a frame reaches the sink within milliseconds of the GPU finishing it instead of at the next draw: two frames behind the viewport at a receiver on the same machine, down from four. Dual view with a live side (a Premiere or After Effects feed) now publishes while QCView's own transport is paused, on both platforms.
- **Output frame rate** — a live feed tells the sink its real rate (a 23.976 Premiere sequence goes out as 23.976, not 25), so receivers pace it correctly.
- **Interface** — the view-mode buttons above the viewport use clearer icons for single, side-by-side, split wipe and difference; the Keyboard Shortcuts card keeps its spacing below the scopes with nothing loaded.
- **Diagnostics** — the live-feed reader's log line now counts frames it never took and duplicates it was handed, and `QCV_BRIDGE_TRACE=1` logs each one.

What's new in 2.5.4

- **Live audio from Premiere Pro** — with QCBridgeAE 0.3.1 and **Audio Stream** ticked for the device in Premiere's Playback preferences, the sequence's audio arrives beside the picture over the bridge: the Inspector's Audio card shows the stream's state, rate and channels with per-channel meters, **Listen in QCView** plays it (off by default — Premiere is playing it already), 5.1 and 7.1 fold to stereo by the same routing rules as files, **Mute scrub audio** applies to it, and an **A/V sync (Live)** offset per host application delays the audio or holds the picture. Live sides in dual view carry their audio into the mix.
- **Dual view** — the per-side mute on the A and B chips is a lit / recessed pill instead of a two-grey icon, and the Inspector's Audio card reads the inspected side's own source.
- **Output** (first step) — Settings ▸ Output lists installed output sinks. Switched on, QCView publishes what the viewport shows (splits, OCIO, guides and notes included) and the mixed audio to that sink as a separate application, only while it is attached, with a feed mute and a choice of included layers. The NDI® sink that uses it is a separate download, coming next; nothing is sent unless an output is on.

What's new in 2.5.3

- **Audio** — playback no longer pops or crackles as it keeps in sync with the picture. Every audio cut (pause, resume, seek, loop wrap, sync correction) is now a short fade, seeks land on the exact sample instead of up to a frame early, audio waits for the first picture after play or seek so it never runs ahead, and the sync servo corrects more gently and re-seeks far less often. A long-standing one-sample-per-block drop in the rate matcher that produced a faint crackle under every servo correction is also fixed. Applies to single clips, dual view and playlists.

- **Safety guides** — new **Meta Feed Vertical 4:5** guide (1080×1350, 250 px top and bottom), and the Meta Reels and Stories guides are now one **Meta 9:16** guide, following Meta's unified Reels / Stories safe zone (14% top, 35% bottom, 6% sides).

What's new in 2.5.2

- **minColor engine** — a second colour engine beside OCIO, built from the minColor core that also powers the minColorAE plugin. Pick **OCIO** or **minColor** in the Color panel's preset bar; the one On / Off switch covers both. The chain is **Input** (gamut + transfer, per clip) → **Highlight Knee** (per clip) → **AgX** → **Rendering** (Un-tone-mapped or OpenDRT) → **Display**. Per-clip pins, badges, presets, screenshots, LUT export and the scopes all work the same way as under OCIO, and each engine keeps its own settings and preset list when you switch.
- **minColor presets** — ACEScg, ACES 2065-1, Linear Rec.709 (with and without AgX), Linear Rec.2020 (with and without AgX) and P3 PQ, each to sRGB for SDR, to EDR P3 (macOS) and to Rec.2100 PQ (Windows). All are un-tone-mapped; OpenDRT is only ever a choice you make in the Rendering reel.
- **Highlight Knee** — a hand-set knee start now always bends smoothly without overshooting (the old curve could dip above the display peak with a late start).
- **Scopes** — a per-clip **Transfer** override pill on video, image sequences and stills for files whose transfer is untagged or wrong, live streams carry their own tags, and the waveform has manual scale chips (**Auto · % · nits**). SDR and scene-referred white sit at 100 nits on the nits scale.
- **Image sequences on Windows** — frame paths with non-Latin characters (CJK and others) load again (#6).
- **Compact Mode** — `Ctrl + Shift + C` (⇧⌘C): the window becomes the picture plus one slim strip with play / pause, the timecode, a scrub line and an exit button, and loses its title bar; drag the timecode to move it. `Esc`, the shortcut again or the strip's ✕ brings every panel back as it was. Minimal Mode (`Ctrl + 0`) is unchanged.
- **Transport bar** — the in and out point icons were the wrong way round.
- **Mercury Transmit** — a producer that quit without closing no longer leaves a stale frame ring behind.

What's new in 2.5.1

- **Dual view: frame stepping** — stepping with ←/→ (or Q/E) no longer re-decodes the whole GOP on the B-frame / long-GOP side on every press, and that side no longer flashes back to the keyframe while it catches up. Both sides land on the exact frame at once. Jumps hold the previous frame until the new one is ready instead of showing the run-up.
- **Project panel** — the loaded item is a rounded blue chip with an outline, the same as the current clip in the Inspector's playlist list; the left accent rule is gone.

What's new in 2.5.0

- **Colour per clip** — each clip now keeps its own Input, Look, Scene LUT and Highlight Knee (the Color panel's **Clip** group). The Output, View and Display LUT (the **View** group) stay shared by everything on screen. In dual view, A and B each render through their own chain; pick a side with the **A** / **B** tabs. Clip chains are saved with the project, and clips that have one show a badge in the project panel and on the A/B source chips.
- **Color panel layout** — three groups, left to right: **Setup** (presets and the OCIO config), **Clip** and **View**. It works the same in single and dual view. A preset sets both halves: its clip half on the selected clip, its view half on the View. ↺ on a clip column goes back to the default.
- **Bulk colour in the project panel** — right-click selected clips for **Input ›** (a searchable picker, with the inputs already used in the project on top), **Use Clip Chain of "…"** and **Reset Clip Chain**.
- **Scopes** — GPU **Vectorscope** and **Waveform** sections in the right rail. The waveform reads in nits on HDR sources (with 203-nit reference white), shows a **Frame · Max** peak readout, and in dual view reads each side from its own file.
- **Highlight Knee** — an optional chain step that compresses highlights above a chosen peak into the display's range, with an amber **KNEE** pill in the viewport while it is active. The Inspector shows the file's HDR metadata (MaxCLL / MaxFALL / mastering peak).
- **Viewer aids** — Exposure (in stops), Gamma and Channel view (RGB / R / G / B / A / luma) for inspecting the source. They are never saved in presets or baked into exports.
- **ASC CDL** — .cc, .ccc and .cdl files load in the Scene LUT slot, with a Correction ID field for collections.
- **Super-whites and sub-blacks kept** — values outside the legal video range now reach OCIO on every decode path (hardware, software and dual view) instead of being clipped. A range chart reads 109 % everywhere; before, software-decoded video and Windows dual view read 100 %.
- **Untagged video is BT.709 on every path** — untagged SD clips no longer decode as BT.601 on some paths and BT.709 on others.
- **Windows: 10-bit video levels** — 10- and 12-bit video no longer leans slightly green (the fix 2.4.1 made on macOS).
- **Windows: dual view** — full frame rate at 4K with far less CPU, and a fix for an intermittent 10–17 second freeze when entering dual view. Dual-view screenshots now capture the dual picture.
- **Menus take typing** — text typed in pop-up menus and pickers no longer triggers app shortcuts (Backspace and Delete could delete the selected project items).

What's new in 2.4.1

- **PQ P3-D65 masters** — a new **ST2084-P3-D65** input in the Blender 5.2 OCIO config for HDR deliverables mastered in P3-D65 with the PQ curve (e.g. Resolve's "P3-D65 ST2084"). Previously these could only be read as Rec.2100-PQ, which treats the P3 values as BT.2020 and oversaturates the greens. The ACES 2.0 config already carries it as `ST2084-P3-D65 - Display`.
- **ST2084-P3 presets** — five new Blender 5.2 presets for PQ P3-D65 masters: sRGB and Rec.1886 (SDR), EDR sRGB (macOS EDR and Windows scRGB), EDR P3 (macOS) and Rec.2100-PQ (Windows HDR10). They convert colorimetrically with no tonemap, so the master is shown as graded; the SDR presets clip above 100 nits.
- **Screenshots and note images in HDR modes** — screenshots, note thumbnails and exported reports taken while the viewport is in an EDR, scRGB or HDR10 mode are now saved as correct SDR sRGB images, using the SDR version of the active view (e.g. ACES 2.0 HDR 1000 nits → ACES 2.0 SDR). Before, they came out dark and clipped (EDR/scRGB) or flat and washed out (HDR10). Notes captured in an HDR mode before 2.4.1 keep their old images.
- **10-bit video levels (macOS)** — hardware-decoded 10-bit video now removes the video-range levels at 10-bit precision. Before, neutrals leaned slightly green, most visibly in HDR shadows.

What's new in 2.4.0

- **Live sources from After Effects and Premiere Pro** over Mercury Transmit with the [QCViewBridgeAE](/qcbridge/adobe/) add-on (separate download).
- The [QCViewBridge for Blender](/qcbridge/blender/) add-on has been upgraded with system tray apps and auto launching Blender on the server systems. (separate download. See the [link](qcbridge-blender.md) for more details.)
- **Live sources in dual view** — an SRT stream or an Adobe host can be either side, or both.
- **Dual view with empty sides** — refinement to dual view behavior so that it’s not longer dependent on having a single media-item loaded first. Dual View can be toggled when empty.
- **Drag the viewport to move the window** ( Can be disabled in Settings → "Drag viewport to move window").
- **Drag into viewport for Dual Views** - Added support to drag media from file browsers or the project bins into either side of the viewport when in Dual View modes.
- **Drag into timelines** - Added drop zones in the timeline tracks to drop media from file browsers or project bins.
- **Review speed** — can speed up or slow down playback.
- **Streaming File Sync Enhancements** — video items trigger a lightweight read-ahead to force streaming services, like LucidLink, to download frames ahead of the playhead.
- **Dual view fixes** — fixed playback after seeking for b-frame media when in a Dual View.
- **App Size** — Builds on macOS and Windows were pruned and file sizes are a bit smaller.

What's new in 2.3.3

- Fixed a regression bug in software decoding of intra-frame media.

What's new in 2.3.2

- Viewport background now shows the bounds of transparent media: the area outside the picture is tinted grey to create a letterbox/pillarbox shape around the media.

What’s new in 2.3.0

- Upgraded from FFmpeg 8.1.2 to 9.0.1 on both platforms
- Added ProRes RAW support
- Added animated WebP support
- Upgraded display path for higher-than-8-bit video sources.
- Improved software decoding of intra-frame Codecs.
- Improved software decoding in Dual View setups.
- Added Vulkan ProRes support in Dual View on Windows (huge performance boost for Windows)

What's new in 2.2.7

- **Avid DNx 4:4:4 levels** — untagged DNxHD/DNxHR 4:4:4 (RGB included)
  is now read as video-level (legal range) by default, matching how Avid
  and After Effects interpret it, so Avid RGB exports come up with true
  black without touching the Range pill. Explicit container tags still
  win, and the Inspector's Range-tag row now shows the container's tag
  separately from what the decoder reported.

What's new in 2.2.6

- **Container & codec detail in the Inspector** — the File card now names
  the container (QuickTime, MP4, MXF…) and, for MXF, the operational
  pattern (**OP1a** vs **OP-Atom**), plus the authoring tool recorded in
  the file.
- **Avid DNx flavors** — DNxHD/DNxHR clips show their real bandwidth name
  (DNxHD 36 / 115 / 145 / 175 / 220, the **x** 10-bit variants, DNxHD 444,
  DNxHR LB / SQ / HQ / HQX / 444) instead of a generic "DNXHD".
- **Bitrate row** — video-stream bitrate (or DNxHD's fixed nominal) with
  the whole-file average alongside, for every container.
- Existing projects pick the new fields up automatically on open via a
  quick header re-probe — no re-import needed.
- **Avid DNxHR 444 fix (macOS)** — DNxHR 444 12/10-bit files written by
  the Avid codec (After Effects / Media Composer) with the adaptive
  colour transform enabled decoded with green/magenta macroblock
  speckle. QCView's FFmpeg now decodes them correctly.
- **Range pill for RGB sources** — Auto now tells you what it resolved
  to (tag, or the assumed convention: YCbCr → limited, RGB → full/as
  stored), the Color card shows the container's range tag, and
  **Limited** expands video-level RGB (16–235) for RGB codecs such as
  DNxHR 444 RGB. MXF RGB essence now reads its range from the
  descriptor.

What's new in 2.2.5

- **Pointer tool for annotations** — select any stroke to move it, scale it
  from the corner handles, or delete it. Full undo support. Opening the
  Notes panel now arms the freehand pen automatically.
- **Go to frame / timecode** — click the frame or timecode readout above
  the transport bar, type a target, and jump straight there. Accepts
  drop-frame timecode and bare digits (`1000` → `00:00:10:00`).
- **Window memory** — QCView now remembers its window size and position
  across launches, with safe recovery when displays change. View ▸
  Default Size & Position resets it.
- Oval annotations can now be erased anywhere on their outline, not just
  near two spots.
- Reopening a video from the bin returns it to the top of Open Recent.
- The timeline toolbar divider no longer cuts across the side margins.

What’s new in 2.2.4
- SRT streaming improvements
- Upgraded the Blender OCIO profile to 5.2 (Includes EDR improvements and several camera profiles to bring it closer to parity with ACES studio packages)

What’s new in 2.2.3
- Added framework to support SRT streaming

What’s new in 2.2.2
- Added a setting to disable audio when scrubbing
- Added a metadata field in the Inspector to show codec specifics such as "ProRes 422 LT"
- Margin tweaks on a few panels
- Added left/right arrow as ALT keyboard shortcuts for previous/next frame

What’s new in 2.2.1
- Automatically rotates mobile videos with portrait orientation metadata
- Added a new option in the inspector panel to manually rotate videos

What’s new in 2.2.0
- added a thumbnail contact sheet to the EXR layer picker in the Inspector panel
- fixed a few bugs around EXR loading and playback
- added a waveform audio visual to the timeline and a setting to toggle it

What’s new in 2.1.10
- UI tweaks and bug fixes

What’s new in 2.1.9
- Improved audio scrubbing. It still pitch-shifts when slower than 1x speed, but no longer pitch-shifts when faster than 1x
- Minimized latency when scrubbing
- Fixed a bug when loading a solitary EXR image.

What’s new in 2.1.8
- Improved audio sync and added pitch-shifting audio for scrubbing + FF/RW 

What’s new in 2.1.5
- tighted up the UI in the timeline, tracks and other elements are less tall
- sidebar rails now fully close
- there is a new setting to always load the app in minimal mode
- a new tooltip and modal system is added, and more tooltips are included

What’s new in 2.1.4

- fixed an issue with odd-width videos with the software scrubbing path on Windows
- added triangle markers on the timeline for notes

What’s new in 2.1.3

- Moved screenshot exporting to its own thread.
- Added options for screenshot exports in settings: PNG, TIFF, and JPEG
- Added messaging on the bottom toolbar for screenshot progress

What’s new in 2.1.1/2.10

- Improved scrubbing for MP4s and other B-frame media
- This release is v2.1.1 on MacOS and v2.1.0 for Windows. Windows didn't need the last-minute regression fix. We will get back in sync for the next release.

What’s new in 2.0.9

- Added track identifiers to the left of the timeline; this is mainly to keep the playhead and grab handles of the zoom/pan tool away from the app’s edges.
- Fixed persistence on image sequence FPS when switching to other media and back.
- Added a dual-view panel for saving dual-view sessions and moved dual-views out of the media panel
- Made the entire surface of collapsed sidebars a trigger to open the rails.
- Removed the max size from the media bin—-eliminating dual scroller contention
- Fixed a glitch where the minimal mode menu option hid the frame counter row
- Added detection for ARRI RAW mxfs and a message to guide users to Resolve or other apps
- Added video range overrides to the scrub decoders (single and dual media flows)
- Added a dark background behind the MacOS icon for light-mode users.

What’s new in 2.08

- Editing in dual-view and playlist modes has better viewport feedback with slips and trims.
- Added a difference mode option for dual-views
- MacOS now checks for and optionally installs updates

What's new in 2.06
- UI tweaks for better panel and selection legibility
- UI tweaks to the playhead in dual-view and playlist edit modes
- Better scrubbing in dual-view modes with video sources
- Added grab handles to the timeline's zoom/pan tool
- Fixed an edge-case crash in playlist mode with thumbnails
- Added a height limit to the thumbnails so vertical videos don't occupy so much viewport space
- Added a loading spinner--especially helpful for large projects that take a moment to load

What's new in 2.04
- Slight UI change to playhead when a timeline track is in edit mode.
- Fixed a bug where thumbnails didn't respect clip edits in dual-view mode.

What's new in 2.03
- Fixed issues in dual view mode when two clips with different FPS were compared.
- Fixed issues with dual view when two ProRes clips were loaded in systems with NVIDIA gpus.

What's new in 2.02
- fixed glitches with windows resizing.
- fixed glitches with the playhead flickering on seek operations.

What's new in 2.01
- new UI and backend - a complete rewrite of the frontend.
- stereo audio mixing for 5.1 SMTPE broadcast master video review.
- added extended linear sRGB as an HDR option for Windows.
- OCIO default presets are now based on app display mode.
- vulkan HW accelerated decoding for ProRes in Windows.
- new dual view and playlist flows. new edit modes for both.
