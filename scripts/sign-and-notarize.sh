#!/bin/bash
# sign-and-notarize.sh — turn a built qcview.app into a signed, notarized,
# stapled QCView-MacOS.dmg ready to attach to a GitHub release.
#
# Order matters and is the usual macOS one: deploy Qt, bundle our own
# dylibs, sign inside-out (nested code first, the app last), build the DMG,
# sign it, notarize, staple.
#
# Requirements on the machine:
#   - Developer ID Application certificate in the login keychain
#     ("Christopher Bialkowski", team 5Z4S9VHV56)
#   - a notarytool keychain profile (default name: QCView), created once with
#       xcrun notarytool store-credentials "QCView" \
#         --apple-id <apple-id> --team-id 5Z4S9VHV56
#     (it then prompts for the app-specific password)
#   - Qt at QT_PREFIX for macdeployqt
#
# Usage: scripts/sign-and-notarize.sh [--skip-notarize] [build-dir]
set -euo pipefail

BUILD_DIR="build"
SKIP_NOTARIZE=0
for arg in "$@"; do
    case "$arg" in
        --skip-notarize) SKIP_NOTARIZE=1 ;;
        *) BUILD_DIR="$arg" ;;
    esac
done

QT_PREFIX="${QT_PREFIX:-$HOME/Qt/6.11.1/macos}"
PROFILE="${NOTARY_PROFILE:-QCView}"
IDENTITY="${CODESIGN_IDENTITY:-Developer ID Application: Christopher Bialkowski (5Z4S9VHV56)}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$REPO/$BUILD_DIR/src/app/qcview.app"
ENTITLEMENTS="$REPO/packaging/macos/entitlements.plist"
DIST="$REPO/$BUILD_DIR/dist"
DMG="$DIST/QCView-MacOS.dmg"          # stable name: the appcast URL uses it

[ -d "$APP" ] || { echo "no app bundle at $APP — build first" >&2; exit 1; }
[ -x "$QT_PREFIX/bin/macdeployqt" ] || { echo "no macdeployqt at $QT_PREFIX" >&2; exit 1; }
security find-identity -v -p codesigning | grep -q "$IDENTITY" \
    || { echo "signing identity not in the keychain: $IDENTITY" >&2; exit 1; }

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "$APP/Contents/Info.plist")"
echo "==> QCView $VERSION  ($APP)"

# ---- 0. clear what earlier runs and dev launches left behind ---------------
# Flat dylibs are ours (bundle_dylibs.sh re-copies them); leaving them means
# shipping whatever an older dependency build put there — a bundle here had
# FFmpeg 8 and 9 side by side. The app also writes its log next to its own
# binary during development, and macdeployqt tries to read that as an object
# file.
echo "==> clean"
rm -f "$APP/Contents/Frameworks"/*.dylib
rm -f "$APP/Contents/MacOS"/*.txt

# ---- 1. Qt frameworks, plugins and QML -------------------------------------
echo "==> macdeployqt"
"$QT_PREFIX/bin/macdeployqt" "$APP" -qmldir="$REPO/src" -verbose=1 \
    | grep -iE "error|warning: .*(not found|could not)" || true

# ---- 2. our own dylibs (FFmpeg, OCIO, OpenEXR, Imath) ----------------------
"$REPO/scripts/bundle_dylibs.sh" "$APP"

# ---- 3. sign, inside-out ---------------------------------------------------
# Nested code first; the app bundle last. Everything gets the hardened
# runtime and a secure timestamp, which notarization requires.
echo "==> codesign"
sign() { codesign --force --options runtime --timestamp --sign "$IDENTITY" "$@"; }

# Sparkle ships its own nested executables (XPC services + Updater.app) and
# they must be signed before the framework that contains them.
SPARKLE="$APP/Contents/Frameworks/Sparkle.framework"
if [ -d "$SPARKLE" ]; then
    find "$SPARKLE" \( -name "*.xpc" -o -name "*.app" \) -maxdepth 3 -print0 2>/dev/null \
        | while IFS= read -r -d '' nested; do sign "$nested"; done
    sign "$SPARKLE/Versions/B" 2>/dev/null || sign "$SPARKLE"
fi

# Dylibs, Qt frameworks, plugins, helper CLIs.
find "$APP/Contents/Frameworks" -name "*.dylib" -print0 2>/dev/null \
    | while IFS= read -r -d '' f; do sign "$f"; done
find "$APP/Contents/Frameworks" -maxdepth 1 -name "Qt*.framework" -print0 2>/dev/null \
    | while IFS= read -r -d '' f; do sign "$f"; done
find "$APP/Contents/PlugIns" "$APP/Contents/Helpers" -type f -perm -u+x -print0 2>/dev/null \
    | while IFS= read -r -d '' f; do sign "$f"; done

# The app itself, with entitlements.
sign --entitlements "$ENTITLEMENTS" "$APP"

echo "==> verify signature"
codesign --verify --deep --strict --verbose=2 "$APP"
spctl --assess --type exec -vv "$APP" || true   # unstapled: "rejected" is expected here

# ---- 4. DMG ----------------------------------------------------------------
echo "==> DMG"
rm -rf "$DIST/stage" "$DMG"
mkdir -p "$DIST/stage"
cp -R "$APP" "$DIST/stage/"
ln -s /Applications "$DIST/stage/Applications"
hdiutil create -volname "QCView $VERSION" -srcfolder "$DIST/stage" \
    -ov -format UDZO -quiet "$DMG"
rm -rf "$DIST/stage"
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

# ---- 5. notarize + staple --------------------------------------------------
if [ "$SKIP_NOTARIZE" -eq 1 ]; then
    echo "==> skipping notarization (--skip-notarize)"
    echo "    $DMG"
    exit 0
fi

echo "==> notarize (this waits for Apple; usually a few minutes)"
xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait

echo "==> staple"
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"
spctl --assess --type open --context context:primary-signature -vv "$DMG"

echo
echo "==> done: $DMG"
echo "    $(shasum -a 256 "$DMG" | awk '{print $1}')"
echo "    next: attach it to the v$VERSION GitHub release, then"
echo "          scripts/update_appcast.sh $VERSION \"$DMG\""
