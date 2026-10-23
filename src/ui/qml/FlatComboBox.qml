import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Flat ComboBox — Theme.bg surface, Theme.border edge,
// Theme.accent on focus / open. Phosphor caret-down indicator.
// Popup is a flat list with Theme.surfaceHover row hover.
ComboBox {
    id: root
    implicitHeight: 26
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSizeSmall

    // Borderless raised-chip styling — value reads as a selected
    // pill. Rest sits at toolbarAlt so the chip stands out clearly
    // against the panel surface (one full step brighter).
    background: Rectangle {
        color: root.activeFocus || root.popup.opened
               ? Theme.borderStrong
               : (root.hovered ? Theme.surfaceHover : Theme.toolbarAlt)
        radius: Theme.radiusSmall
        Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
    }

    contentItem: Text {
        text: root.displayText
        color: Theme.textPrimary
        font: root.font
        verticalAlignment: Text.AlignVCenter
        leftPadding: Theme.spacing
        rightPadding: root.indicator.width + Theme.spacing
        elide: Text.ElideRight
    }

    indicator: Item {
        x: root.width - width - Theme.spacing
        y: 0
        width: 16
        height: root.height
        Icon {
            anchors.centerIn: parent
            name: "caret-down"
            size: Theme.iconSizeSmall
            color: Theme.textSecondary
        }
    }

    popup: Popup {
        y: root.height
        width: root.width
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
        padding: 3
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.popupMs } }
        background: Rectangle {
            color: Theme.surface
            border.color: Theme.border
            border.width: 1
            radius: Theme.radius
        }
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { }
        }
    }

    delegate: ItemDelegate {
        width: ListView.view ? ListView.view.width : root.width
        height: 24
        contentItem: Text {
            text: modelData
            color: Theme.textPrimary
            font: root.font
            verticalAlignment: Text.AlignVCenter
            leftPadding: Theme.spacing
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: parent.hovered || parent.highlighted
                   ? Theme.surfaceHover : Theme.hoverIdle
            Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
        }
    }
}
