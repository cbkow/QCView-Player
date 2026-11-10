---
title: Playlists
permalink: /playlists/
nav_order: 14
---

# Playlists

To create a playlist, click the `+` button to the right of the Playlists header in the Left Rail's Media section. The new playlist loads straight away, with its name ready to type. You can also select media items, right-click and choose **Create Playlist from Selection**.

To build a playlist, drag video or image-sequence sources from the Media section onto the timeline, or onto the clip bar under it: drop near the edge of a clip to insert there, anywhere else to add at the end. Files dragged in from the desktop work the same way. Audio-only files are not accepted. Mixed video + image-sequence playlists work, and play straight through from one clip to the next.

<!-- SHOT qcv3_048: The + button in the Playlists header of the Media section, and a loaded playlist: its list of clips under its row in the Left Rail and the clip bar under the timeline -->

While a playlist is loaded, its row in the Media section opens into a list of its clips. Click a clip to jump to it, drag a clip to reorder, and drag media items into the list to add them (see [Project Panel](/project-manager/#a-loaded-playlists-clips)).

---

## Rearrange

Drag the body of a clip on the clip bar to move it to a new place in the order; an insertion bar shows where it will land. There is no edit mode to enter. Clicking a clip seeks to it and selects it. You can also reorder by dragging in the playlist's list of clips in the Media section.

---

## Per-clip trims

Drag either end of a clip on the clip bar to trim its head or tail; a badge shows the change in frames. `Ctrl` / `⌘` + drag slips the source under the clip. Double-click a clip for exact values: **Source in**, **Source out** and **Duration**, typed as timecode. `Enter` applies them and `Esc` discards them.

Per-clip in/out trims are honored across the playlist boundary — the next clip starts at its trim head on auto-advance.

| Action | macOS | Windows |
|---|---|---|
| Split the clip at the playhead | `⌘ + K` | `Ctrl + K` |
| Delete the selected clip | `Delete` / `Backspace` | `Delete` / `Backspace` |
| Undo / redo an edit | `⌘ + Z` / `⇧ + ⌘ + Z` | `Ctrl + Z` / `Ctrl + Shift + Z` |

Every edit is written back to the playlist in the project.

<!-- SHOT qcv3_049: The playlist clip bar with a clip's end being trimmed (the frame badge showing), and the Source in / Source out / Duration box open over another clip -->

---

## Playback

The usual transport controls play the playlist as one timeline, and with loop on it starts again from the first clip. To jump between clips, click a clip on the clip bar or in the playlist's list in the Media section. QCView 3.0 has no Prev clip / Next clip buttons.
