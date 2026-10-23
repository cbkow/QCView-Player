import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Flat TextField — the family's editable field (minNotes' frame title,
// QCView's KvRow wells): a recessed well (surfaceRecess, which reads on
// bg, surface and card alike) with a 1-px border that steps up on hover
// and turns accent on focus. No vertical padding: the Basic style's 6 px
// each side left a 10-px line box in a 22-px field and clipped 11-px
// text — the line centres in whatever height the caller gives.
TextField {
    id: root

    implicitHeight: 22
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSizeSmall
    color: Theme.textPrimary
    placeholderTextColor: Theme.textMuted
    selectByMouse: true
    topPadding: 0
    bottomPadding: 0
    leftPadding: Theme.spacing + 2
    rightPadding: Theme.spacing + 2
    opacity: enabled ? 1.0 : 0.55

    background: Rectangle {
        color: Theme.surfaceRecess
        radius: Theme.radiusSmall
        border.width: 1
        border.color: root.activeFocus ? Theme.accent
                    : root.hovered ? Theme.borderStrong : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.hoverMs } }
    }
}
