import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Flat slim slider matching ufb's style: 2-px-tall track in
// borderStrong, accent-coloured fill from origin to handle,
// 10×10 squared handle in accent (accentHover while pressed).
//
// Use:
//   FlatSlider { from: 0.0; to: 1.0; value: 0.5; onMoved: ... }
Slider {
    id: root
    // Optional group tint (the Color panel's Setup / Clip / View
    // colours): greys lean toward it, the accent becomes it.
    // Transparent = the stock look.
    property color tint: "transparent"
    function tinted(c, amount) {
        return tint.a > 0 ? Qt.tint(c, Qt.rgba(tint.r, tint.g, tint.b, amount)) : c;
    }
    readonly property color tintAccent: tint.a > 0 ? tint : Theme.accent

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width:  root.availableWidth
        height: 2
        // Track stays subtly visible when disabled — its color
        // already reads as low contrast so we leave it.
        color:  root.tinted(Theme.borderStrong, 0.35)
        Rectangle {
            width:  root.visualPosition * parent.width
            height: parent.height
            // Fill dims to textMuted when the slider is disabled
            // (e.g. volume slider with no audio loaded).
            color:  root.enabled ? root.tinted(Theme.textBright, 0.35)
                                 : root.tinted(Theme.textMuted, 0.25)
        }
    }
    handle: Rectangle {
        x: root.leftPadding
           + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 10; height: 10
        color: !root.enabled
                 ? root.tinted(Theme.textMuted, 0.25)
                 : root.tinted(root.pressed ? Theme.textPrimary : Theme.textBright, 0.35)
    }
    // Faint overall fade for the whole control when disabled —
    // matches FlatButton's disabled affordance.
    opacity: root.enabled ? 1.0 : 0.55
}
