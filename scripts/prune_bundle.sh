#!/bin/bash
# prune_bundle.sh — drop what macdeployqt deployed and QCView never uses.
#
# macdeployqt is deliberately generous: it follows Qt's own dependencies, so
# one QML plugin pulling in QtMultimedia is enough to land Qt's entire media
# stack — including a SECOND FFmpeg beside ours — in the DMG. QCView's QML
# imports only QtQuick, QtQuick.Controls (Fusion style), QtQuick.Dialogs,
# QtQuick.Layouts, QtQuick.Window, QtCore and our own Qcv modules.
#
# Two passes:
#   1. remove the modules nothing imports, by name (below)
#   2. garbage-collect: drop any framework no remaining binary links, to a
#      fixpoint. This is what makes it safe — step 1 only has to be roughly
#      right, because anything still referenced survives step 2.
#
# QML plugins load dynamically, so a plugin is a root, not a leaf: if it
# stays, whatever it links stays too.
#
# Usage: scripts/prune_bundle.sh <path/to/qcview.app>
set -euo pipefail

APP="${1:?usage: prune_bundle.sh <app>}"
FW="$APP/Contents/Frameworks"
PI="$APP/Contents/PlugIns"
QML="$APP/Contents/Resources/qml"
[ -d "$FW" ] || { echo "prune: no Frameworks in $APP" >&2; exit 1; }

before=$(du -sm "$APP" | awk '{print $1}')

# ---- 1. modules QCView does not import -------------------------------------
# Qt3D / Pdf / Multimedia / VirtualKeyboard / Sql / StateMachine, and every
# Controls style except the one the app sets (Fusion) plus Basic, which
# Fusion builds on.
PRUNE_FRAMEWORKS=(
    Qt3DAnimation Qt3DCore Qt3DExtras Qt3DInput Qt3DLogic
    Qt3DQuick Qt3DQuickScene2D Qt3DQuickScene3D Qt3DRender
    QtMultimedia QtPdf QtPdfQuick
    QtVirtualKeyboard QtVirtualKeyboardQml QtVirtualKeyboardSettings
    QtSql QtQmlLocalStorage QtStateMachine QtStateMachineQml
    QtQuickControls2FluentWinUI3StyleImpl
    QtQuickControls2Imagine QtQuickControls2ImagineStyleImpl
    QtQuickControls2IOSStyleImpl QtQuickControls2MacOSStyleImpl
    QtQuickControls2Material QtQuickControls2MaterialStyleImpl
    QtQuickControls2Universal QtQuickControls2UniversalStyleImpl
)
PRUNE_PLUGIN_DIRS=(multimedia sceneparsers geometryloaders renderers sqldrivers)
# Qt's own FFmpeg, deployed with its media plugin. Ours are 63 / 63 / 61 / 12
# / 63 / 7 / 10 — do not touch those.
PRUNE_DYLIBS=(
    libavcodec.61.dylib libavformat.61.dylib libavutil.59.dylib
    libswresample.5.dylib libswscale.8.dylib
)
PRUNE_QML=(
    QtMultimedia QtQuick3D Qt3D QtQuick/VirtualKeyboard QtQuick/Pdf
    QtQuick/Controls/Material QtQuick/Controls/Universal
    QtQuick/Controls/Imagine QtQuick/Controls/FluentWinUI3
    QtQuick/Controls/iOS QtQuick/Controls/macOS
    QtQuick/LocalStorage QtQml/StateMachine
)

for f in "${PRUNE_FRAMEWORKS[@]}"; do rm -rf "$FW/$f.framework"; done
for d in "${PRUNE_PLUGIN_DIRS[@]}"; do rm -rf "$PI/$d"; done
for l in "${PRUNE_DYLIBS[@]}"; do rm -f "$FW/$l"; done
for q in "${PRUNE_QML[@]}"; do rm -rf "$QML/$q"; done

# The QML plugins for what we just removed (they live with the Quick ones).
for p in vkb virtualkeyboard pdfquick qmllocalstorage qtqmlstatemachine \
         controls2material controls2universal controls2imagine \
         controls2fluentwinui3 controls2ios controls2macos; do
    find "$PI" -name "*${p}*plugin.dylib" -delete 2>/dev/null || true
done

# ---- 2. garbage-collect unreferenced frameworks ----------------------------
# Roots: the app binary, the helper CLIs, and every plugin (dynamically
# loaded, so each is a root). Anything reachable from them stays.
gc_pass() {
    local removed=0 fwname keep
    for fwdir in "$FW"/*.framework; do
        [ -d "$fwdir" ] || continue
        fwname="$(basename "$fwdir" .framework)"
        [ "$fwname" = "Sparkle" ] && continue          # the updater, always kept
        keep=0
        while IFS= read -r bin; do
            case "$bin" in "$fwdir"/*) continue ;; esac   # its own files
            if otool -L "$bin" 2>/dev/null | grep -q "/$fwname.framework/"; then
                keep=1; break
            fi
        done < <(find "$APP/Contents/MacOS" "$APP/Contents/Helpers" "$PI" "$FW" \
                      -type f \( -perm -u+x -o -name "*.dylib" \) 2>/dev/null)
        if [ "$keep" -eq 0 ]; then
            echo "    unreferenced: $fwname"
            rm -rf "$fwdir"
            removed=1
        fi
    done
    return $removed
}
echo "  garbage-collecting frameworks"
while ! gc_pass; do :; done     # repeat until a pass removes nothing

after=$(du -sm "$APP" | awk '{print $1}')
echo "prune: ${before} MB -> ${after} MB"
