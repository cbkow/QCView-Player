---
title: Technical Overview
permalink: /technical-overview/
nav_order: 3
---

# Technical Overview

A quick orientation to what's under the hood. Nothing here changes how you use the app — skip to the feature pages if you're after a specific control.

## Stack

QCView 3 is written in Rust and draws everything — the picture and the interface — on the GPU through [wgpu](https://wgpu.rs/). There is no UI framework underneath: the interface is QCView's own toolkit, drawn as one vector scene and composited over the finished video.

| | Windows | macOS |
|---|---|---|
| **UI** | Own toolkit: vello (2D), parley (text), winit (window) | Same |
| **GPU** | Direct3D 12 through wgpu | Metal through wgpu |
| **HW decode** | FFmpeg: D3D12VA for inter-frame codecs such as h.264 and h.265, Vulkan for ProRes, FFV1 and APV (with software fallback) | FFmpeg: VideoToolbox (with software fallback) |
| **Audio** | WASAPI shared mode, MMCSS "Pro Audio" priority | CoreAudio default-output AudioUnit |
| **Display surface** | 10-bit sRGB on an SDR monitor; 16-bit float scRGB on an HDR monitor | 16-bit float, extended linear sRGB (EDR) |

Media goes through FFmpeg 9; OCIO colour through OpenColorIO 2.6; EXR through the OpenEXRCore library. **About → Licenses…** lists every bundled component.

## Building blocks

The code is a set of libraries with one application on top. The ones that shape what you see:

| Part | Job |
|---|---|
| **Presenter** (`min-present`) | Takes decoded frames with their colour metadata and draws them: decode to a linear working image, the colour chain, the display transform, then the interface on top. Also draws dual view, the waveform, screenshots and the frames sent to outputs. |
| **FFmpeg layer** (`min-ffmpeg`) | Opens files and streams, indexes frames for exact seeks, picks the hardware decoder per codec, reads ahead. |
| **Image loaders** (`min-image`) | EXR, TIFF, PNG, JPEG, DPX, TGA and BMP frames. |
| **Player** (`qcv-player`) | The transport: the clock, the playback and scrub decoders, the image-sequence cache, dual view's two sides, audio, live feeds, thumbnails. |
| **Colour** (`qcv-color`) | The two engines, minColor and OCIO, each as a GPU pass that plugs into the presenter. |
| **Interface** (`min-ui`, `min-theme`) | The widgets and the design tokens, fonts and icons. |
| **Files** (`qcv-project`, `qcv-notes`, `qcv-meta`, `qcv-mxf`) | The `.qcvproj` project file, the notes sidecar, and media metadata (including the Adobe XMP and MXF header readers). The project and notes formats are the ones QCView 2 wrote, kept unchanged. |
| **QCBridge link** (`qcv-ring`) | The shared-memory rings that carry frames and audio to and from the QCBridge companions. |

## The picture's path

Every frame takes the same four steps on the GPU:

1. **Decode to linear.** The frame's planes (YUV or RGB, integer or float) become linear working RGB, using the file's own range, matrix, primaries and transfer tags — or your Range / Transfer override from the Inspector.
2. **Colour chain.** With Color on, the clip's chain (minColor or OCIO) runs on a 16-bit half-float image at the video's own size.
3. **Display transform.** The result is fitted to the window and encoded for the surface the window is on.
4. **Interface.** The UI layer is blended over the finished video at SDR white.

When no colour chain is in play, steps 1 and 3 run as a single pass and no working image is made.

Hardware-decoded frames stay on the GPU. On macOS, VideoToolbox frames are used directly where their pixel layout allows. On Windows, D3D12VA decodes on the same Direct3D 12 device that draws, and the Vulkan-decoded codecs (ProRes, FFV1, APV) are copied GPU-to-GPU into Direct3D 12 textures without passing through system memory. Software-decoded frames and image-sequence frames are uploaded as planes.

QCView 2's pipeline modes (Normal, High-Res, Ultra-High-Res, HDR) are gone: the working image a colour chain runs on is always half float, whatever the source's bit depth.

## HDR

QCView has no display mode list. The surface is picked from the screen, and brightness is *relative* on both systems: `1.0` is the screen's SDR white, and HDR highlights go above it into whatever headroom the display has. Nits are not literal. When an HDR picture is shown on a screen with no headroom, a pill in the Color panel says its highlights clip; the Highlight Knee brings them into range.

**Windows**: the surface follows the monitor that holds most of the window. On an HDR monitor (Windows HDR on) it is a 16-bit float scRGB surface, and the picture is scaled by the monitor's "SDR content brightness" setting so SDR white matches the desktop. On an SDR monitor it is a 10-bit sRGB surface and the chain ends in SDR. Moving the window between the two switches the surface; you may see a one-frame blip as it crosses.

**macOS**: one 16-bit float EDR surface (extended linear sRGB), where `1.0 = SDR white` and values above 1.0 are brighter, up to the display's headroom.

OCIO chains follow the same rule: QCView builds the display end itself from the config's views, so you pick the config, Input, Look and View, and never an output device.

## Playback

Video is the master clock; the transport runs on rational time, so frame numbers and timecode stay exact at fractional rates.

- **Playback decoder**: a streaming decoder with read-ahead. A frame index (PTS ↔ frame number, keyframes) is built for each file so seeks and steps land on the exact frame.
- **Scrub decoder**: a second decoder with its own FFmpeg session, used while you drag the playhead, so scrubbing never disturbs the playback decoder's position. Decoded frames near the playhead are kept in the scrub cache (**Settings → Performance**).
- **Dual view**: one master clock at the faster side's rate drives two sides, each a complete transport with its own decoders and colour chain. A live source on a side is not clocked; it shows its newest frame.
- **Playlists**: clips are resolved against one timeline, with seeks across clip boundaries.

## Audio

WASAPI on Windows and CoreAudio on macOS, float32 stereo at 48 kHz, always on the system's default output device. The mixer is shared across platforms.

Audio follows the video clock: a small servo trims the resampling ratio to hold sync rather than jumping, and re-seeks only when drift gets large. Review speeds keep their pitch (SoundTouch). Files with more than two channels are folded to stereo by the **Mix** picker in the Inspector: Auto, 5.1 → 2, or channels 7-8.

## Image sequences

EXR (multi-layer and multi-part, with layer extraction), TIFF, PNG, JPEG, DPX, TGA and BMP. EXR frames arrive as half floats in linear light; the others as 8- or 16-bit integers.

Frames are loaded into a read-ahead cache around the playhead: the playhead's frame first, then the frames ahead, then the frames behind, with frames outside the cache window evicted. The window size and the number of decode threads are in **Settings → Performance**. A missing file or a frame that fails to decode holds the last good picture, and the timeline shows the hole.

## Color

Two engines, one chain per clip (see [OCIO Color](/color/) and [minColor](/mincolor/)):

- **minColor**, the default, is the minColorAE core compiled to a single GPU compute pass. Every control is a shader parameter, so a change never recompiles anything.
- **OCIO** runs in-process: OpenColorIO builds the chain and generates its shader, and that shader runs natively (Metal on macOS, Direct3D 12 on Windows). No LUT is baked for display. Bundled configs: ACES 2.0, ACES 1.3, Blender 5.2; a config file of your own can be opened too.

The waveform reads the source through the clip's Input, not the finished picture. Screenshots, note images, thumbnails and notes exports go through the clip's chain, rendered for SDR.

## Live feeds and outputs

- **After Effects and Premiere Pro** (QCBridgeAE) hand frames and audio to QCView through shared memory on the same machine; QCView always shows the newest frame.
- **SRT and other network streams**, including a QCBridge Blender session, are opened through FFmpeg on their own thread and shown newest-first, never paced by timestamps.
- **Outputs** (NDI through QCBridgeN, AJA through QCBridgeA) are separate companion applications. QCView links neither SDK: it publishes the viewport picture and the mixed audio into shared memory, at the raster and transfer the output asks for, and the companion sends it on. Frames carry timecode and the paused state.

## Files and settings

- Projects are `.qcvproj` files; notes are sidecar files in a `.qcview` folder alongside the media. Both are the QCView 2 formats, so 2.x projects and notes open as they are and minNotes reads the same notes.
- Settings are one JSON file per user: `~/Library/Application Support/QCView/settings.json` on macOS, under `%APPDATA%\QCView` on Windows. Logs go to `~/Library/Logs/QCView` and `%LOCALAPPDATA%\QCView\logs`.
- A second launch hands its files and `qcview://` links to the window that is already open.
