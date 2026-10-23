import QtQuick
import QtQuick.Controls
import Qcv

// Project-themed Menu. Since the humanist pass (2026-10-23) its rows are
// ThemedMenuItem (declare that type inline too, never a plain MenuItem):
// the look no longer depends on the Fusion palette (which follows the OS
// colour scheme on Windows: the 2.5.9 light-mode label bug). Rounded like the other popups, the rows
// rounded like buttons, the row fill ramping over Theme.hoverMs, the
// menu fading in over Theme.popupMs.
//
// popupType stays Popup.Window: the popup must z-order above the player
// child HWND on Windows (F.2.1). A popup window is created transparent,
// so the rounded corners show what is behind them.
Menu {
    id: themedMenu
    popupType: Popup.Window
    padding: 3
    overlap: 1
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.popupMs } }

    background: Rectangle {
        implicitWidth:  200
        implicitHeight: 24
        color:        Theme.surface
        border.color: Theme.border
        border.width: 1
        radius:       Theme.radius
    }

    delegate: ThemedMenuItem { }

    // The palette still reaches anything the delegate does not draw (a
    // plain MenuItem a caller drops in, a MenuSeparator).
    palette.window:           Theme.surface
    palette.windowText:       Theme.textPrimary
    palette.base:             Theme.surface
    palette.text:             Theme.textPrimary
    palette.button:           Theme.surface
    palette.buttonText:       Theme.textPrimary
    palette.highlight:        Theme.affordanceHover   // grey, like the bar (chris, 2026-10-21)
    palette.highlightedText:  Theme.textBright
    palette.mid:              Theme.border
    palette.midlight:         Theme.surfaceHover
    palette.dark:             Theme.bg
    palette.shadow:           Theme.bg
    palette.disabled.text:       Theme.textMuted
    palette.disabled.windowText: Theme.textMuted
    palette.disabled.buttonText: Theme.textMuted
}
