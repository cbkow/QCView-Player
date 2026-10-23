import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Flat SpinBox — FlatTextField's well with caret arrows.
// Softly rounded, Theme.border at rest, Theme.accent on focus.
// Up / down arrows use Phosphor carets, hover to accent.
SpinBox {
    id: root

    implicitHeight: 22
    font.family: Theme.monoFamily
    font.pixelSize: Theme.fontSizeSmall
    // No vertical padding (see FlatTextField): the value centres in
    // the field instead of being clipped by the style's 6-px padding.
    topPadding: 0
    bottomPadding: 0

    // The field well, as FlatTextField draws it.
    background: Rectangle {
        color: Theme.surfaceRecess
        radius: Theme.radiusSmall
        border.width: 1
        border.color: root.activeFocus ? Theme.accent
                    : root.hovered ? Theme.borderStrong : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.hoverMs } }
    }

    contentItem: TextInput {
        text: root.displayText
        font: root.font
        color: Theme.textPrimary
        horizontalAlignment: Qt.AlignLeft
        verticalAlignment: Qt.AlignVCenter
        readOnly: !root.editable
        validator: root.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        leftPadding: Theme.spacing
        rightPadding: Theme.spacing
        selectByMouse: true
    }

    up.indicator: Rectangle {
        x: root.mirrored ? 0 : root.width - width
        height: root.height / 2
        implicitWidth: 18
        color: root.up.pressed
                ? Theme.accentMuted
                : (upMa.containsMouse ? Theme.surfaceHover : "transparent")
        border.width: 0
        Icon {
            anchors.centerIn: parent
            name: "caret-up"
            size: 10
            color: upMa.containsMouse ? Theme.accent : Theme.textSecondary
        }
        MouseArea {
            id: upMa
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton  // SpinBox handles clicks internally
        }
    }

    down.indicator: Rectangle {
        x: root.mirrored ? 0 : root.width - width
        y: root.height / 2
        height: root.height / 2
        implicitWidth: 18
        color: root.down.pressed
                ? Theme.accentMuted
                : (downMa.containsMouse ? Theme.surfaceHover : "transparent")
        border.width: 0
        Icon {
            anchors.centerIn: parent
            name: "caret-down"
            size: 10
            color: downMa.containsMouse ? Theme.accent : Theme.textSecondary
        }
        MouseArea {
            id: downMa
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }
    }
}
