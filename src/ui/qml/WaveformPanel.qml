// WaveformPanel — the luma waveform (WindowManager.scope, see
// src/window/scope_controller.h). Column × level: for SDR sources the
// file's own Y′, −10 %…110 % — sub-blacks and super-whites stay visible
// and are tinted red; for HDR / linear / log sources the vectorscope's
// interpretation as linear luminance in nits, 0 to the chosen peak
// (300 / 600 / 1k / 2k / 4k), HDR reference white (203) in amber — levels above it pile up at the top, red.
// Placement-agnostic; sizes to its width.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

ColumnLayout {
    id: root
    readonly property var scope: WindowManager.scope
    spacing: Theme.spacing

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        Text {
            Layout.fillWidth: true
            text: root.scope ? root.scope.waveformBadge : ""
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
            font.bold: true
            // Two lines: where the reading came from ("built-in config",
            // the A / B pair) is the part that must not be cut.
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
        // Signal values, not this screen's light: PQ is absolute, SDR
        // white sits at its 100-nit reference. The display shows both at
        // whatever its own settings say (macOS EDR, Windows SDR slider).
        Text {
            Layout.alignment: Qt.AlignTop
            text: !root.scope ? "" : (root.scope.waveformHdr ? qsTr("nits · reference") : qsTr("% Y′"))
            color: Theme.textMuted
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeMono
            HoverHandler { id: unitHover }
            FlatToolTip {
                visible: unitHover.hovered && root.scope && root.scope.waveformHdr
                text: qsTr("What the file encodes on a reference display, not what this "
                           + "screen emits. SDR white = 100 nits (BT.1886 reference).")
            }
        }
    }

    Item {
        id: face
        Layout.fillWidth: true
        // Taller than wide-screen: the linear nits scale needs the room.
        Layout.preferredHeight: Math.round(width * 0.9)

        Rectangle {
            anchors.fill: parent
            color: "#0b0b0b"
            radius: Theme.radiusSmall
        }
        Image {
            anchors.fill: parent
            source: root.scope ? root.scope.waveformImageSource : ""
            cache: false
            smooth: true
            fillMode: Image.Stretch
        }
        Canvas {
            id: lines
            anchors.fill: parent
            renderStrategy: Canvas.Cooperative
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                if (!root.scope || width <= 0) return;
                const ls = root.scope.waveformLines;
                ctx.font = "9px sans-serif";
                for (let i = 0; i < ls.length; ++i) {
                    const y = Math.round(ls[i].y * height) + 0.5;
                    ctx.strokeStyle = ls[i].ref ? "rgba(217,164,65,0.55)"
                                    : ls[i].major ? "rgba(255,255,255,0.32)"
                                                  : "rgba(255,255,255,0.16)";
                    ctx.beginPath();
                    ctx.moveTo(0, y);
                    ctx.lineTo(width, y);
                    ctx.stroke();
                    // Label above its line; below it at the top edge.
                    const top = y < 12;
                    ctx.textBaseline = top ? "top" : "bottom";
                    ctx.fillStyle = ls[i].ref ? "rgba(217,164,65,0.9)" : "rgba(255,255,255,0.6)";
                    ctx.fillText(ls[i].label, 3, top ? y + 2 : y - 1);
                }
            }
            Connections {
                target: root.scope
                function onStateChanged() { lines.requestPaint() }
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Component.onCompleted: requestPaint()
        }
        Row {
            visible: root.scope && root.scope.dual
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 6
            spacing: 8
            Text { text: "A"; color: "#4dd9ff"; font.family: Theme.monoFamily; font.pixelSize: Theme.fontSizeMono; font.bold: true }
            Text { text: "B"; color: "#ff9e40"; font.family: Theme.monoFamily; font.pixelSize: Theme.fontSizeMono; font.bold: true }
        }
    }

    // Peak readout: this frame and the max so far, per side; measured
    // over every source pixel. Brightest channel in the tooltip.
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        visible: root.scope && root.scope.waveformPeaks.length > 0
        Column {
            Layout.fillWidth: true
            spacing: 1
            Repeater {
                model: root.scope ? root.scope.waveformPeaks : []
                Text {
                    required property var modelData
                    width: parent.width
                    textFormat: Text.StyledText
                    text: (modelData.side.length > 0
                           ? "<b><font color='" + (modelData.side === "A" ? "#4dd9ff" : "#ff9e40")
                             + "'>" + modelData.side + "</font></b>  " : "")
                          + qsTr("Frame") + " <b>" + modelData.frame + "</b>  ·  "
                          + qsTr("Max") + " <b>" + modelData.clip + "</b>"
                          + (root.scope.waveformHdr ? " " + qsTr("nits") : "")
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeMono
                    elide: Text.ElideRight
                    HoverHandler { id: peakHover }
                    FlatToolTip {
                        visible: peakHover.hovered
                        text: modelData.tooltip
                    }
                }
            }
        }
        FlatChip {
            label: qsTr("Reset")
            tooltip: qsTr("Start Max over")
            onClicked: root.scope.resetClipPeaks()
        }
    }

    // Scale (HDR) and trace brightness (shared with the vectorscope).
    RowLayout {
        Layout.fillWidth: true
        spacing: 2
        // Manual scale: Auto follows the interpretation (tags / Transfer /
        // Input); % and nits force it. Shared with the vectorscope.
        Repeater {
            model: [
                { key: 0, label: qsTr("Auto"), tip: qsTr("Scale from the interpretation") },
                { key: 1, label: "%",          tip: qsTr("Force the SDR percent scale (the signal's own Y′)") },
                { key: 2, label: qsTr("nits"), tip: qsTr("Force the nits scale (SDR white at 203)") },
            ]
            FlatChip {
                required property var modelData
                label: modelData.label
                minWidth: 26
                active: root.scope && root.scope.scaleMode === modelData.key
                tooltip: modelData.tip
                onClicked: root.scope.scaleMode = modelData.key
            }
        }
        Rectangle {
            visible: root.scope && root.scope.waveformHdr
            Layout.preferredWidth: 1
            Layout.preferredHeight: 14
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            color: Theme.divider
        }
        Repeater {
            model: root.scope && root.scope.waveformHdr ? [300, 600, 1000, 2000, 4000] : []
            FlatChip {
                required property int modelData
                label: modelData >= 1000 ? (modelData / 1000) + "k" : String(modelData)
                minWidth: 26
                active: root.scope && root.scope.waveformPeak === modelData
                tooltip: qsTr("Top of scale %1 nits").arg(modelData)
                onClicked: root.scope.waveformPeak = modelData
            }
        }
        Item { Layout.fillWidth: true }
        FlatSlider {
            Layout.preferredWidth: 70
            from: 0.25
            to: 4.0
            value: root.scope ? root.scope.brightness : 1.0
            onMoved: root.scope.brightness = value
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Trace brightness")
        }
    }
}
