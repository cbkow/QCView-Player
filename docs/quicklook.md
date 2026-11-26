---
title: Quick Look
permalink: /quicklook/
nav_order: 15.5
---

# Quick Look (macOS)

New in 3.0.5. QCView adds a Quick Look preview for two kinds of file that Finder shows little or nothing for: OpenEXR pictures and MXF video. Select a file in Finder and press `Space`. There is nothing to switch on; the previews come with the app and work whether or not QCView is running.

Move the pointer over the preview and one bar appears along the bottom. It goes away again when the pointer rests or leaves.

## EXR

An EXR opens as a picture, with its size, compression and bit depth on the bar. Two pickers sit beside them:

- **Layer**: every layer in the file, and every part of a multi-part file. A layer without red, green and blue, such as depth or a matte, is shown in grey.
- **Color**: *Color Off* (the picture as QCView shows it with no color chain) or one of [minColor](/mincolor/)'s built-in presets.

A choice carries to the next file in the same folder, so Finder's arrow keys step through the frames of a render on one layer and one look. Quick Look does not play a sequence; open it in QCView for that.

## MXF

An MXF plays by itself, with sound. The bar holds:

| Control | Key | Action |
|---|---|---|
| Rewind | hold `J` or `A` | Runs backwards while held, faster the longer it is held |
| Play / pause | `K`, `W` or `S` | A click on the picture does the same |
| Fast-forward | hold `L` or `D` | Runs forwards while held, faster the longer it is held |
| Playbar | | Click or drag to scrub |
| Timecode | | The file's own timecode |
| Mute | `M` | Remembered for the next file |
| Color | | As for an EXR |

`Q` and `E` step one frame, `Home` and `End` go to the first and last frame. The keys reach the preview after one click on the picture; `Space`, the arrows and `Esc` stay Quick Look's own.

In Finder's smaller previews (the preview column, gallery view) an MXF is a silent still with a play button that appears under the pointer.

MXF files also get their first frame as their icon in Finder.

## Limits

- QuickTime and MPEG-4 files (`.mov`, `.mp4`) stay with macOS's own preview, including the ones it cannot decode: macOS does not hand those types to another app. Open such a file in QCView.
- A double click on any Quick Look preview opens the file in its default app. That is Quick Look's behavior, not the bar's.
- The previews use built-in presets only. They do not read QCView's settings or OCIO configs.
