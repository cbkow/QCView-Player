// CompactStrip — the one piece of chrome in Compact Mode.
//
// A fixed 22 px band under the viewport (never over it: the viewport is
// a native surface and in-scene QML cannot draw above it, and a strip
// whose height changed on hover would resize that surface). Timecode
// play / pause and the timecode on the left, a scrub line with the
// playhead and the in / out marks through the middle, expand (exit) on the right
// (fullscreen stays on F — chris, 2026-10-07). Scrubbing runs
// the timeline panel's own gesture (scrubBeginAt / scrubMoveTo /
// scrubEndAt), so dual, playlist, image-sequence and audio sources all
// behave as they do on the timeline. Live sources show a live dot.
// The window itself drops its title bar in Compact Mode (Main.qml →
// WindowManager.setCompactBorderless), so the timecode doubles as the
// move handle and the edges still resize.

import QtQuick
import QtQuick.Layouts
import Qcv

Rectangle {
    id: root
    objectName: "compactStrip"
    color: Theme.surface

    // The TimelinePanel instance whose scrub gesture this strip drives.
    property var timeline: null

    signal exitRequested()

    readonly property var  timer: WindowManager.timeline ? WindowManager.timeline.timer : null
    readonly property real duration: timer ? timer.duration : 0
    readonly property real position: timer ? timer.position : 0
    readonly property bool loaded: duration > 0 && !WindowManager.liveActive
    readonly property bool isPlaylistMode:
        WindowManager.timeline && WindowManager.timeline.sourceMode === 1
    // The same reading TransportBar makes: dual master, else the timer
    // for image sequences and audio, else the video decoder.
    readonly property bool isPlaying: {
        if (WindowManager.dualController) return WindowManager.dualController.isPlaying;
        if (WindowManager.imageSeqActive || WindowManager.audioActive) return timer ? timer.playing : false;
        return !!WindowManager.videoDecoder && WindowManager.videoDecoder.isPlaying;
    }

    // The active clock's fps (dual master / video source / timeline),
    // the same choice TimelineStatus makes, for the in / out marks.
    readonly property real clockFps: {
        if (WindowManager.dualController) return WindowManager.dualController.fps;
        if (WindowManager.videoDecoder && !WindowManager.imageSeqActive
            && WindowManager.videoDecoder.fps > 0) return WindowManager.videoDecoder.fps;
        return timer ? timer.frameRate : 0;
    }

    function fmtSec(s) {
        if (!isFinite(s) || s < 0) s = 0;
        const totalS = Math.floor(s);
        const sec = totalS % 60, min = Math.floor(totalS / 60) % 60, hr = Math.floor(totalS / 3600);
        const ss = sec.toString().padStart(2, "0"), mm = min.toString().padStart(2, "0");
        return (hr > 0 ? hr + ":" + mm : min + "") + ":" + ss;
    }

    Rectangle {
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: Theme.dividerWidth
        color: Theme.divider
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.padding
        anchors.rightMargin: Theme.paddingLoose   // clear of the rounded corner
        spacing: Theme.paddingLoose

        // ---- Play / pause -------------------------------------------
        FlatButton {
            iconName: root.isPlaying ? "pause" : "play"
            iconColor: Theme.textBright
            iconSize: Theme.iconSizeToolbar
            Layout.preferredWidth: 22
            Layout.preferredHeight: 22
            enabled: root.loaded
            tooltipText: root.isPlaying ? qsTr("Pause (Space)") : qsTr("Play (Space)")
            onClicked: WindowManager.togglePlayback()
        }

        // ---- Timecode / time — also the window's move handle -------
        // The window has no title bar in Compact Mode; dragging here
        // moves it through the OS (startSystemMove: AppKit / Win32).
        Item {
            Layout.preferredWidth: 96
            Layout.fillHeight: true
            Layout.leftMargin: Theme.padding    // air between the button and the digits
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.OpenHandCursor
                onPressed: (m) => { if (Window.window) Window.window.startSystemMove(); }
                FlatToolTip { visible: parent.containsMouse; text: qsTr("Drag to move the window") }
            }
        Text {
            anchors.fill: parent
            text: {
                const _refresh = root.position;
                if (WindowManager.liveActive) return qsTr("LIVE");
                if (!root.timer || root.duration <= 0) return "";
                if (root.isPlaylistMode) return root.fmtSec(root.position) + " / " + root.fmtSec(root.duration);
                if (WindowManager.dualController)
                    return WindowManager.dualController.formatTimecode(
                        Math.max(0, WindowManager.dualController.currentFrame));
                if (WindowManager.videoDecoder && !WindowManager.imageSeqActive
                    && WindowManager.videoDecoder.fps > 0) {
                    const _originTag = WindowManager.videoDecoder.startTimecode;
                    return WindowManager.videoDecoder.formatTimecode(
                        Math.max(0, WindowManager.videoDecoder.currentFrame));
                }
                const fc = WindowManager.frameCountUnified();
                return fc > 0 ? qsTr("%1 / %2").arg(WindowManager.currentFrameUnified()).arg(fc - 1)
                              : root.fmtSec(root.position);
            }
            color: WindowManager.liveActive ? Theme.accent : Theme.textPrimary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeSmall
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        }

        // ---- Scrub line ---------------------------------------------
        Item {
            id: track
            Layout.fillWidth: true
            Layout.fillHeight: true

            readonly property real fraction:
                root.duration > 0 ? Math.max(0, Math.min(1, root.position / root.duration)) : 0
            function xToSeconds(x) {
                return root.duration * Math.max(0, Math.min(1, x / Math.max(1, width)));
            }

            // The line
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 2
                color: scrubMa.containsMouse || scrubMa.pressed ? Theme.borderStrong : Theme.divider
            }
            // Played part
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: root.loaded ? parent.width * track.fraction : 0
                height: 2
                color: Theme.textSecondary
            }
            // In / out marks
            Repeater {
                model: root.loaded && WindowManager.hasInOutRange && !root.isPlaylistMode
                       && root.clockFps > 0 && root.duration > 0
                       ? [WindowManager.inPoint, WindowManager.outPoint] : []
                delegate: Rectangle {
                    required property int modelData
                    x: Math.max(0, Math.min(track.width - width,
                           track.width * (modelData / root.clockFps) / root.duration))
                    anchors.verticalCenter: parent.verticalCenter
                    width: 2; height: 10
                    color: Theme.accent
                }
            }
            // Playhead
            Rectangle {
                visible: root.loaded
                x: Math.max(0, Math.min(track.width - width, track.width * track.fraction - width / 2))
                anchors.verticalCenter: parent.verticalCenter
                width: 3; height: 12
                color: Theme.textPrimary
            }
            // Live dot
            Rectangle {
                visible: WindowManager.liveActive
                anchors.verticalCenter: parent.verticalCenter
                x: 0
                width: 8; height: 8; radius: 4
                color: Theme.accent
            }

            MouseArea {
                id: scrubMa
                anchors.fill: parent
                hoverEnabled: true
                enabled: root.loaded && !!root.timeline
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onPressed: (m) => root.timeline.scrubBeginAt(track.xToSeconds(m.x))
                onPositionChanged: (m) => { if (pressed) root.timeline.scrubMoveTo(track.xToSeconds(m.x)); }
                onReleased: (m) => root.timeline.scrubEndAt(track.xToSeconds(m.x))
            }
        }

        // ---- Expand back to the full UI -----------------------------
        FlatButton {
            iconName: "arrows-out-simple"
            iconSize: Theme.iconSizeToolbar
            Layout.preferredWidth: 22
            Layout.preferredHeight: 22
            tooltipText: qsTr("Exit Compact Mode (Esc)")
            onClicked: root.exitRequested()
        }
    }
}
