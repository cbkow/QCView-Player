// ScopePanel — the vectorscope face and its controls (WindowManager.scope,
// see src/window/scope_controller.h). Placement-agnostic: it sizes to
// its width (square face), so it can live in the right rail today and a
// pop-out window later.
//
// The trace is a GPU image (image://qcvscope/<serial>, 512², allowed to
// lag); the graticule is drawn here on a Canvas, Tek / Resolve
// orientation (Cb →, Cr ↑). Which layers show depends on the tier:
//   Signal  — circle, 10° ticks, centre crosshair (the neutral check)
//   Assumed — + dimmed colour-bar targets and the skin-tone line
//   Input   — + full targets and the 709 / P3 / 2020 gamut hexagons

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

ColumnLayout {
    id: root
    readonly property var scope: WindowManager.scope
    spacing: Theme.spacing

    // ---- Badge: tier + colour space, scale
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        Text {
            Layout.fillWidth: true
            text: root.scope ? root.scope.badge : ""
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
            font.bold: true
            elide: Text.ElideRight
        }
        Text {
            text: root.scope ? root.scope.scaleLabel : ""
            color: Theme.textMuted
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeMono
        }
    }
    Text {
        visible: root.scope && root.scope.mismatch.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.scope ? root.scope.mismatch : ""
        color: Theme.warning
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeTiny
    }

    // ---- Face
    Item {
        id: face
        Layout.fillWidth: true
        Layout.preferredHeight: width

        Rectangle {
            anchors.fill: parent
            color: "#0b0b0b"
            radius: Theme.radiusSmall
        }
        Image {
            anchors.fill: parent
            source: root.scope ? root.scope.imageSource : ""
            cache: false
            smooth: true
            asynchronous: false
            fillMode: Image.Stretch
        }
        Canvas {
            id: graticule
            anchors.fill: parent
            renderStrategy: Canvas.Cooperative

            function dashedPolyline(ctx, pts, closed, dash, gap) {
                const n = pts.length / 2;
                const segs = closed ? n : n - 1;
                for (let i = 0; i < segs; ++i) {
                    const x0 = pts[2 * i], y0 = pts[2 * i + 1];
                    const j = (i + 1) % n;
                    const x1 = pts[2 * j], y1 = pts[2 * j + 1];
                    const len = Math.hypot(x1 - x0, y1 - y0);
                    if (dash <= 0) {
                        ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke();
                        continue;
                    }
                    for (let t = 0; t < len; t += dash + gap) {
                        const a = t / len, b = Math.min(t + dash, len) / len;
                        ctx.beginPath();
                        ctx.moveTo(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a);
                        ctx.lineTo(x0 + (x1 - x0) * b, y0 + (y1 - y0) * b);
                        ctx.stroke();
                    }
                }
            }

            onPaint: {
                const ctx = getContext("2d");
                const w = width, h = height;
                ctx.reset();
                if (!root.scope || w <= 0) return;
                const cx = w / 2, cy = h / 2, r = w / 2 - 1;
                const tier = root.scope.tier;
                const line = "rgba(255,255,255,0.28)";

                // Circle + 10° ticks.
                ctx.strokeStyle = line;
                ctx.lineWidth = 1;
                ctx.beginPath();
                ctx.arc(cx, cy, r, 0, 2 * Math.PI);
                ctx.stroke();
                for (let d = 0; d < 360; d += 10) {
                    const a = d * Math.PI / 180;
                    const inner = d % 30 === 0 ? r - 7 : r - 4;
                    ctx.beginPath();
                    ctx.moveTo(cx + inner * Math.cos(a), cy - inner * Math.sin(a));
                    ctx.lineTo(cx + r * Math.cos(a), cy - r * Math.sin(a));
                    ctx.stroke();
                }
                // Centre crosshair — neutral lands here in every tier.
                ctx.strokeStyle = "rgba(255,255,255,0.45)";
                ctx.beginPath();
                ctx.moveTo(cx - 8, cy); ctx.lineTo(cx + 8, cy);
                ctx.moveTo(cx, cy - 8); ctx.lineTo(cx, cy + 8);
                ctx.stroke();
                if (tier === 0) return;

                // Skin-tone line.
                const skin = root.scope.skinLine;
                if (skin.length === 2) {
                    ctx.strokeStyle = "rgba(230,180,140,0.55)";
                    ctx.beginPath();
                    ctx.moveTo(cx, cy);
                    ctx.lineTo(skin[0] * w, skin[1] * h);
                    ctx.stroke();
                }

                // Gamut hexagons (Input tier): 709 solid, P3 dashed, 2020 dotted.
                const hexes = root.scope.hexagons;
                for (let k = 0; k < hexes.length; ++k) {
                    const hx = hexes[k];
                    const pts = [];
                    for (let i = 0; i < hx.points.length; i += 2) {
                        pts.push(hx.points[i] * w, hx.points[i + 1] * h);
                    }
                    ctx.strokeStyle = hx.name === "709" ? "rgba(255,255,255,0.40)"
                                    : hx.name === "P3" ? "rgba(140,200,255,0.45)"
                                                       : "rgba(255,200,120,0.45)";
                    dashedPolyline(ctx, pts, true,
                                   hx.name === "709" ? 0 : (hx.name === "P3" ? 6 : 2),
                                   hx.name === "P3" ? 4 : 3);
                }

                // Colour-bar targets: 100 % boxes (labelled), 75 % small boxes.
                const targets = root.scope.targets;
                ctx.globalAlpha = tier === 1 ? 0.5 : 0.9;
                ctx.font = "9px sans-serif";
                for (let i = 0; i < targets.length; ++i) {
                    const t = targets[i];
                    const x = t.x * w, y = t.y * h;
                    const s = t.full ? 10 : 6;
                    ctx.strokeStyle = t.full ? "rgba(255,255,255,0.8)" : "rgba(255,255,255,0.55)";
                    ctx.strokeRect(x - s / 2, y - s / 2, s, s);
                    if (t.full) {
                        // Label on the centre side of its box — 100 %
                        // targets sit on the circle's edge.
                        const dx = x - cx, dy = y - cy;
                        const len = Math.max(Math.hypot(dx, dy), 1);
                        ctx.fillStyle = "rgba(255,255,255,0.75)";
                        ctx.textAlign = "center";
                        ctx.textBaseline = "middle";
                        ctx.fillText(t.label, x - dx / len * 16, y - dy / len * 16);
                    }
                }
                ctx.globalAlpha = 1.0;
            }

            Connections {
                target: root.scope
                function onStateChanged() { graticule.requestPaint() }
            }
            onWidthChanged: requestPaint()
            Component.onCompleted: requestPaint()
        }

        // Dual overlay legend.
        Row {
            visible: root.scope && root.scope.dual
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 6
            spacing: 8
            Text { text: "A"; color: "#4dd9ff"; font.family: Theme.monoFamily; font.pixelSize: Theme.fontSizeMono; font.bold: true }
            Text { text: "B"; color: "#ff9e40"; font.family: Theme.monoFamily; font.pixelSize: Theme.fontSizeMono; font.bold: true }
        }
    }

    // ---- Controls
    RowLayout {
        Layout.fillWidth: true
        spacing: 2
        Repeater {
            model: [1, 2, 4]
            FlatChip {
                required property int modelData
                label: modelData + "×"
                minWidth: 26
                active: root.scope && root.scope.zoom === modelData
                tooltip: qsTr("Zoom %1× — magnifies the centre").arg(modelData)
                onClicked: root.scope.zoom = modelData
            }
        }
        Item { Layout.preferredWidth: Theme.spacing }
        FlatChip {
            label: qsTr("Color")
            active: root.scope && root.scope.colorize
            interactive: root.scope && root.scope.tier > 0
            tooltip: root.scope && root.scope.tier > 0
                     ? qsTr("Colour the trace by hue")
                     : qsTr("Signal tier draws mono — the colour space isn't known")
            onClicked: if (interactive) root.scope.colorize = !root.scope.colorize
        }
        FlatChip {
            label: qsTr("Persist")
            active: root.scope && root.scope.persistence
            tooltip: qsTr("Phosphor-style persistence")
            onClicked: root.scope.persistence = !root.scope.persistence
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
