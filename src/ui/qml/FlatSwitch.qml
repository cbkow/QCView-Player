import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Flat squared switch — track + sliding square handle.
//   off → track Theme.surface, handle Theme.textMuted
//   on  → track Theme.accentMuted, handle Theme.accent
// Set `attention: true` to flip the OFF state to an accent-blue
// fill — useful for surfacing toggles the user is meant to notice
// (e.g. "Engage OCIO" when the chain isn't applied).
// No animation curve, instant snap.
Switch {
    id: root
    property bool attention: false
    // Optional group tint (the Color panel's Setup / Clip / View
    // colours): greys lean toward it, the accent becomes it.
    // Transparent = the stock look.
    property color tint: "transparent"
    function tinted(c, amount) {
        return tint.a > 0 ? Qt.tint(c, Qt.rgba(tint.r, tint.g, tint.b, amount)) : c;
    }
    readonly property color tintAccent: tint.a > 0 ? tint : Theme.accent
    implicitWidth: 32
    implicitHeight: 18
    spacing: 0
    // Kill the Basic-style template's default ~6 px padding — the
    // indicator draws at x: leftPadding, so any padding painted the
    // track OUTSIDE the control's 32x18 logical bounds. That made
    // right-anchored switches (Settings' SlotSwitch column) overhang
    // the control column by the padding amount. Zero padding =
    // visual footprint === geometry, so anchors mean what they say.
    padding: 0

    indicator: Rectangle {
        implicitWidth: 32
        implicitHeight: 18
        x: root.leftPadding
        y: parent.height / 2 - height / 2
        radius: 0
        // Track shifts a notch brighter when on; the handle
        // colour is the strong differentiator (bright vs muted).
        // attention mode paints OFF with accent so the toggle
        // visually pulls the eye while disengaged, and ON with
        // success so the engaged state reads as confirmed.
        color: root.attention
               ? (root.checked ? Theme.success : Theme.accent)
               : root.tinted(root.checked ? Theme.borderStrong : Theme.surface, 0.18)
        border.color: root.attention
                      ? (root.checked ? Qt.lighter(Theme.success, 1.15)
                                      : Theme.accentHover)
                      : root.tinted(root.checked ? Theme.textMuted : Theme.border, 0.30)
        border.width: 1

        Rectangle {
            x: root.checked ? parent.width - width - 2 : 2
            y: 2
            width: parent.height - 4
            height: parent.height - 4
            radius: 0
            color: root.checked
                   ? Theme.textBright
                   : (root.attention ? Theme.textBright : root.tinted(Theme.textMuted, 0.25))
        }
    }

    contentItem: Text {
        leftPadding: root.indicator.width + Theme.spacing
        text: root.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
    }
}
