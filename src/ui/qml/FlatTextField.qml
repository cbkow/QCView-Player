import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Flat borderless TextField — recessed-slot styling. Solid fill
// reads against the panel surface (Theme.bgAlt is one shade
// darker than Theme.surface). Focus brightens the fill and adds
// a thin accent bottom rule. No border outline.
TextField {
    id: root
    // Optional group tint (the Color panel's Setup / Clip / View
    // colours): greys lean toward it, the accent becomes it.
    // Transparent = the stock look.
    property color tint: "transparent"
    function tinted(c, amount) {
        return tint.a > 0 ? Qt.tint(c, Qt.rgba(tint.r, tint.g, tint.b, amount)) : c;
    }
    readonly property color tintAccent: tint.a > 0 ? tint : Theme.accent

    implicitHeight: 22
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSizeSmall
    color: Theme.textPrimary
    placeholderTextColor: Theme.textMuted
    selectByMouse: true
    leftPadding: Theme.spacing
    rightPadding: Theme.spacing

    background: Rectangle {
        // Rest sits a full step darker than the panel so the slot
        // reads clearly. Hover lifts halfway, focus matches the
        // panel surface and adds the accent underline.
        color: root.tinted(root.activeFocus
                           ? Theme.surface
                           : (root.hovered ? Theme.bgAlt : Theme.bg), 0.10)
        radius: 0
        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.activeFocus ? root.tintAccent : "transparent"
        }
    }
}
