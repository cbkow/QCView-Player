#!/bin/bash
# bundle_dylibs.sh — make qcview.app self-contained.
#
# Copies every non-system dylib the app and its Helpers link (FFmpeg, OCIO,
# OpenEXR, Imath) into Contents/Frameworks and rewrites the load commands to
# @rpath / @executable_path so nothing resolves to a developer's
# external/install or to Homebrew.
#
# It does NOT run macdeployqt — sign-and-notarize.sh does that first, and
# this script runs after, because macdeployqt rewrites paths of its own.
#
# There is deliberately no `vtool` step. Until 2026-09-22 this script
# rewrote the minimum-OS load command of Homebrew's libpng / libjpeg /
# libtiff, which are built for a far newer macOS than QCView targets (13.0).
# That silenced the loader without making the calls inside safe. Those three
# are now vendored static (dependencies.md), and FFmpeg is built without
# X11/xcb, so every dylib here is one we built at the 13.0 floor. If this
# script ever finds a Homebrew path, that is a bug in the dependency build,
# not something to paper over: it stops instead.
#
# Usage: scripts/bundle_dylibs.sh [path/to/qcview.app]
set -euo pipefail

APP="${1:-build/src/app/qcview.app}"
[ -d "$APP" ] || { echo "bundle_dylibs: no app bundle at $APP" >&2; exit 1; }

FRAMEWORKS="$APP/Contents/Frameworks"
HELPERS="$APP/Contents/Helpers"
mkdir -p "$FRAMEWORKS"

# A dylib we have to carry: anything not from the OS, not already inside the
# bundle, and not a Qt framework (macdeployqt owns those).
is_vendored() {
    case "$1" in
        /usr/lib/*|/System/*|@rpath/*|@executable_path/*|@loader_path/*) return 1 ;;
        *) return 0 ;;
    esac
}

deps_of() {   # absolute paths of a binary's non-system links
    # otool -L lists the binary's OWN install name first, and Qt frameworks
    # deployed by macdeployqt refer to each other by paths inside the bundle;
    # neither is something to copy.
    local bin="$1" appabs
    appabs="$(cd "$APP" && pwd)"
    otool -L "$bin" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        case "$dep" in "$appabs"/*) continue ;; esac
        is_vendored "$dep" && echo "$dep"
    done
}

copied=""
copy_dep() {                       # copy once, recursing into its own deps
    local src="$1" base
    base="$(basename "$src")"
    case " $copied " in *" $base "*) return 0 ;; esac

    case "$src" in
        /opt/homebrew/*|/usr/local/*)
            echo "bundle_dylibs: $base comes from Homebrew ($src)." >&2
            echo "  Every shipped dylib must be built at the 13.0 floor —" >&2
            echo "  see dependencies.md. Refusing to bundle it." >&2
            exit 1 ;;
    esac

    [ -f "$src" ] || { echo "bundle_dylibs: missing $src" >&2; exit 1; }
    cp -f "$src" "$FRAMEWORKS/$base"
    chmod u+w "$FRAMEWORKS/$base"
    install_name_tool -id "@rpath/$base" "$FRAMEWORKS/$base"
    copied="$copied $base"

    local dep
    while read -r dep; do
        [ -n "$dep" ] || continue
        copy_dep "$dep"
        install_name_tool -change "$dep" "@rpath/$(basename "$dep")" \
            "$FRAMEWORKS/$base"
    done < <(deps_of "$src")
}

retarget() {                       # point a binary at the bundled copies
    local bin="$1" rpath="$2" dep
    while read -r dep; do
        [ -n "$dep" ] || continue
        copy_dep "$dep"
        install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$bin"
    done < <(deps_of "$bin")
    install_name_tool -add_rpath "$rpath" "$bin" 2>/dev/null || true
}

echo "bundle_dylibs: $APP"
retarget "$APP/Contents/MacOS/qcview" "@executable_path/../Frameworks"

# The ffmpeg / ffprobe CLIs QCView shells out to (probe, thumbnails).
if [ -d "$HELPERS" ]; then
    for tool in "$HELPERS"/*; do
        [ -f "$tool" ] && [ -x "$tool" ] || continue
        retarget "$tool" "@executable_path/../Frameworks"
    done
fi

# Nothing may still point outside the bundle.
echo "bundle_dylibs: verifying"
leaks=0
while read -r bin; do
    while read -r dep; do
        [ -n "$dep" ] || continue
        echo "  LEAK  $(basename "$bin") -> $dep" >&2
        leaks=1
    done < <(deps_of "$bin")
done < <(find "$APP/Contents/MacOS" "$HELPERS" "$FRAMEWORKS" -type f -perm -u+x 2>/dev/null)
[ "$leaks" -eq 0 ] || { echo "bundle_dylibs: unbundled dependencies above" >&2; exit 1; }

echo "bundle_dylibs: ok — $(ls "$FRAMEWORKS" | wc -l | tr -d ' ') items in Frameworks"
