import QtQuick
import QtQuick.Controls
import Qcv

// The row of a ThemedMenu: ThemedMenu's delegate for Action-made items
// and the type to declare inline (an explicit MenuItem would be drawn
// by Fusion instead — different padding, a mnemonic underline, OS
// palette colours). Label, check mark, submenu caret and the ramped
// hover fill all come from the Theme tokens.
MenuItem {
    id: mi
    implicitHeight: 26
    implicitWidth: Math.max(200, label.implicitWidth + 24 + 32)
    leftPadding: 24; rightPadding: 24; topPadding: 0; bottomPadding: 0
    readonly property color fg: !mi.enabled ? Theme.textMuted
                              : mi.highlighted ? Theme.textBright : Theme.textPrimary
    contentItem: Text {
        id: label
        // The text may carry a mnemonic ("E&xit"); the letter shows
        // plain. "&&" is a literal ampersand.
        text: mi.text.replace(/&&/g, "\u0001").replace(/&/g, "").replace(/\u0001/g, "&")
        color: mi.fg
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeBase
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Item {
        x: 7; width: 12; height: mi.height
        Icon { anchors.centerIn: parent; name: "check"; size: 11; color: mi.fg
               visible: mi.checkable && mi.checked }
    }
    arrow: Item {
        x: mi.width - width - 8; width: 10; height: mi.height
        Icon { anchors.centerIn: parent; name: "caret-right"; size: 10; color: mi.fg
               visible: !!mi.subMenu }
    }
    background: Rectangle {
        radius: Theme.radiusSmall
        // The same grey as the menu bar's open-item highlight (chris, 2026-10-21).
        color: mi.highlighted ? Theme.affordanceHover : Theme.hoverIdle
        Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
    }
}
