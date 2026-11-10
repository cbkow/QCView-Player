---
title: Privacy Policy
permalink: /privacy/
nav_order: 17
---

# Privacy Policy

**QCView 3** — Last updated: November 2026

## Summary

QCView does not collect, store, or transmit any personal data. All application data remains on your device.

## Data Collection

QCView does not collect analytics, telemetry, crash reports, or usage statistics. No data is sent to us or any third party automatically.

## Local Data Storage

QCView stores the following data locally on your device:

- **macOS**: `~/Library/Application Support/QCView/` (settings), `~/Library/Logs/QCView/` (logs)
- **Windows**: `%APPDATA%\QCView\` (settings), `%LOCALAPPDATA%\QCView\` (logs, and a copy of the bundled FFmpeg tools for the QCBridge add-ons)

Stored data includes:

- Application settings and preferences, including the lists of recent media and projects
- A log of the current and the previous session, for troubleshooting. It can contain the paths of files you opened. It is never sent anywhere
- Project files, annotation notes, strokes and exports, wherever you choose to save them

This data never leaves your device unless you explicitly choose to share it (for example, by exporting an annotation report or copying a project file to a shared drive).

## Network Activity

The only automatic network connection QCView makes is the software-update check described below. It does not send analytics, telemetry, usage data, or any of your media or project content.

The **About** menu's **Docs** item opens this site in your default web browser; **Licenses…** opens a page stored inside the app. QCView itself does not fetch or transmit any data through these items.

### Connections you start

Some features use the network because that is what they are for. They do nothing until you turn them on, and they carry only the picture and sound involved:

- **File ▸ Open Stream…** connects to, or listens at, the stream address you enter.
- **File ▸ Connect to Blender** listens for a QCBridge session on this machine while it is switched on. It is off at every launch.
- **The NDI output** (the separate QCBridge NDI companion, if you have installed it) announces itself and sends the viewer's picture and sound on your local network while it is enabled.

## Software Updates (macOS)

On macOS, QCView uses the [Sparkle](https://sparkle-project.org/) framework to check for new versions. By default it checks once per day, and you can also check manually from the **About** menu. You can turn automatic checks off in **Settings → Updates**.

When a check runs, QCView requests the update feed at `https://qcview.app/appcast.xml` and, if an update is available, the installer from GitHub. As with any web request, the server receives your IP address, and the request includes your current app version and macOS version so the correct update can be offered. QCView does **not** send an anonymized system profile, and no other information is transmitted. Updates are cryptographically verified (EdDSA signature + Apple code signature) before installation.

The Windows version updates through the Microsoft Store.

## Third-Party Services

QCView does not integrate with any advertising, analytics, or tracking services.

## Children's Privacy

QCView does not knowingly collect any information from anyone, including children under the age of 13.

## Changes to This Policy

If this policy changes, the updated version will be published at the same location with a revised date.

## Contact

If you have questions about this privacy policy, please open an issue at:
[https://github.com/cbkow/QCView-Player/issues](https://github.com/cbkow/QCView-Player/issues).
