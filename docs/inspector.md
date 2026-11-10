---
title: Inspector
permalink: /inspector/
nav_order: 7
---

# Inspector

The Inspector lives in the Right Rail (`Ctrl + 2`). It shows per-source properties and provides quick paths to the source files. The Right Rail starts collapsed on a fresh install; open it with the shortcut or the caret tab at the right edge of the picture.

The Inspector is laid out in groups — **File**, **Video**, **Geometry**, **Color**, **Audio**, **Timecode**, **Linked Projects** and, for image sequences, **Image Sequence** — each of which can be collapsed. A thumbnail of the clip sits at the top of the File group, drawn in the clip's own colour. In dual view the Inspector follows the targeted side.

<!-- SHOT qcv3_025: The Inspector in the Right Rail with a video loaded: the thumbnail and the File, Video, Geometry and Color groups expanded -->

## Scopes

The waveform is no longer in the Inspector: it is a card in the Color panel, and QCView 3.0 has no vectorscope. See the [Color](/color/) page.

## Path actions

The File group shows the name and folder of the currently-loaded media, and ends with two path actions:

| Button | Action |
|---|---|
| Copy path | Copies the path to the clipboard (native separators on each OS) |
| Reveal | Opens the containing folder in Finder / Explorer and selects the file |

For an image sequence both act on the sequence's first frame. **Reveal** is greyed, with the tooltip "Not reachable from this machine", when the file is not there. A live item has **Copy URL** in their place; the URL is shown and copied without its passphrase.

## File and video details

The **File** group names the container family (QuickTime, MP4, MXF, Matroska…) and, for MXF, the operational pattern — **OP1a** (interleaved, what Adobe Media Encoder / Premiere / Resolve write) versus **OP-Atom** (Avid's one-essence-per-file layout) — plus the authoring tool when the file records one, alongside the file's size and duration.

The **Video** group shows the resolution, frame rate, total frames, pixel format and bit depth, and the codec with its flavor in export-menu terms: ProRes 422 / HQ / 4444, and for Avid VC-3 the Compression ID is read from the first frame so you get the actual bandwidth name — **DNxHD 36 / 115 / 145 / 175 / 220…**, the **x** suffix marking the 10-bit variants (175x, 220x), **DNxHD 444**, and the resolution-independent **DNxHR LB / SQ / HQ / HQX / 444** classes. A **Bitrate** row lists the video stream's rate when the container declares it, with the whole-file average alongside. An **HDR** row appears for HDR content, and **Decode** names the hardware decoder in use, or says *software*.

Projects saved before these fields existed are back-filled on open with a header-only re-probe — no full re-scan.

## Per-clip properties

Several properties are stored per media item and set with rows of **chips** in the Inspector:

- **Aspect** (Geometry) — how the pixels are shaped. **Square** shows the stored pixels untouched and is the default; **Detected** uses the file's pixel aspect ratio (the chip names it, e.g. "Detected 2:1", when the file is anamorphic); **Custom** opens two fields, **Pixel aspect** and **Display aspect**, to type your own. The **Detected** and **Display** rows underneath show what the file declares and what the viewport ends up showing.
- **Rotation** (Geometry) — **Auto** follows the file's rotation tag and says what it found (e.g. "Auto (90°)"); **0°**, **90°**, **180°** and **270°** force one.
- **Range** (Color) — force the source to be read as limited or full range, overriding container metadata. Useful for sources that are mis-tagged. **Auto** follows the tag and says what it resolved to; when the file carries no tag, Auto falls back to the convention for the pixel class and says so on the chip — "Auto (Limited, assumed)" for YCbCr sources, "Auto (Full, assumed)" for RGB sources. Pick **Limited** on an RGB source to expand video-level RGB (16–235) to full. The **Range tag** row above shows the container's tag (or "untagged") and, when the container is silent, where the effective range came from (e.g. "decoder: limited").
- **Transfer** (Color) — tell QCView how to read a source whose transfer tags are missing or wrong: **Auto** follows the file's tags (the chip says what Auto resolves to, e.g. "Auto (SDR, untagged)"), or pick **SDR 709**, **PQ 2020**, **PQ P3**, **HLG** or **Linear**. Untagged sources read as SDR, so an HDR export without tags needs this. Live items have the same chips, and image sequences and stills have them in the Image Sequence group, where Auto means scene-linear for EXR and sRGB for everything else.
- **Mix** (Audio) — shown when the file has six or more audio channels: **Auto**, **5.1 → 2** to preview the 5.1 downmixed to stereo, and, with eight or more channels, **7-8** to listen to the stereo pair on channels 7 and 8.
- **Origin** (Timecode) — pick which timecode drives the status strip's readout: **From start** to ignore embedded timecode and count from zero, **Embedded**, and, when the file carries them, **QT Start**, **QT TC** and **XMP Alt**.

Aspect, Rotation, Range, Transfer and Mix are kept with the media item: they persist with the project on save and re-apply on open. Origin is a choice for the current session. Screenshots apply the Aspect, Rotation, Range and Transfer choices, and thumbnails follow Range and Transfer.

<!-- SHOT qcv3_026: The Inspector's Geometry and Color groups showing the Aspect, Rotation, Range and Transfer chip rows -->

## Audio

The **Audio** group lists the codec, sample rate, channel count and layout, the **Mix** chips described above, and a level meter per channel.

<!-- SHOT qcv3_027: The Inspector's Audio and Timecode groups for a multi-channel file: the Mix chips, the per-channel meters and the Origin chips -->

## Linked Projects

The **Linked Projects** group lists the After Effects and Premiere Pro projects a render was made from, read from the file's embedded metadata. Each project has three buttons: **Open** to open it in its application, **Copy** to copy its path, and **Reveal** to show it in Finder / Explorer. Open and Reveal are greyed, with the tooltip "Not reachable from this machine", when the path does not exist on your machine.

<!-- SHOT qcv3_028: The Linked Projects group showing an After Effects project path with its Open, Copy and Reveal buttons -->

## Image Sequences

For image sequences, the **Image Sequence** group displays the frame range, file name pattern, resolution, duration and, for EXR, the compression. It holds the **Frame rate** chips, the **Transfer** chips, and the **Cache stride** chips (`1x 2x 3x 4x`) for heavy sequences that won't play back without help, with **Cache ahead** and **Cache behind** readouts underneath. A multi-layer EXR lists its layers as thumbnails at the bottom; click one to view it. See [Image Sequences](/image-sequences/).

<!-- SHOT qcv3_029: The Image Sequence group for a multi-layer EXR sequence: Frame rate, Transfer and Cache stride chips, the cache readouts and the layer thumbnails -->

## Live sources

For a live item the Inspector shows the stream's name and URL, the resolution, frame rate and pixel format of what is arriving, the **Transfer** chips, and the feed's audio with its meters.
