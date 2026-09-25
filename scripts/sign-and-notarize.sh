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
BUILT_APP="$REPO/$BUILD_DIR/src/app/qcview.app"   # what CMake produces
ENTITLEMENTS="$REPO/packaging/macos/entitlements.plist"
DIST="$REPO/$BUILD_DIR/dist"
DMG="$DIST/QCView-MacOS.dmg"          # stable name: the appcast URL uses it

[ -d "$BUILT_APP" ] || { echo "no app bundle at $BUILT_APP — build first" >&2; exit 1; }
[ -x "$QT_PREFIX/bin/macdeployqt" ] || { echo "no macdeployqt at $QT_PREFIX" >&2; exit 1; }
security find-identity -v -p codesigning | grep -q "$IDENTITY" \
    || { echo "signing identity not in the keychain: $IDENTITY" >&2; exit 1; }

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "$BUILT_APP/Contents/Info.plist")"
echo "==> QCView $VERSION"

# ---- 0. stage a fresh copy -------------------------------------------------
# Everything below happens on a COPY, never on what CMake built. macdeployqt
# and install_name_tool rewrite the bundle in place, so deploying twice over
# the same bundle means the second run starts from the first run's output —
# which is how an earlier attempt here ended up with Qt's plugins from a
# previous deploy still inside, and macdeployqt hunting for libraries a
# clean step had removed. A copy also leaves the dev bundle runnable.
APP="$DIST/qcview.app"
echo "==> stage"
rm -rf "$DIST"
mkdir -p "$DIST"
cp -R "$BUILT_APP" "$APP"

# ---- 1. Qt frameworks, plugins and QML -------------------------------------
echo "==> macdeployqt"
# The old form piped straight into grep and ended `|| true`, which quietened
# grep's "no matches" — and, with pipefail, a macdeployqt FAILURE along with
# it. Signing then went ahead over an incomplete bundle. Log it, check the
# status, then grep the log.
DEPLOY_LOG="$DIST/macdeployqt.log"
if ! "$QT_PREFIX/bin/macdeployqt" "$APP" -qmldir="$REPO/src" -verbose=1 \
        > "$DEPLOY_LOG" 2>&1; then
    echo "macdeployqt failed — $DEPLOY_LOG" >&2
    grep -iE "error|not found|could not" "$DEPLOY_LOG" | tail -20 >&2 || true
    exit 1
fi
grep -iE "error|warning: .*(not found|could not)" "$DEPLOY_LOG" || true

# ---- 2. our own dylibs (FFmpeg, OCIO, OpenEXR, Imath) ----------------------
"$REPO/scripts/bundle_dylibs.sh" "$APP"

# ---- 2b. drop what QCView never loads --------------------------------------
# Before signing: pruning a signed bundle invalidates it.
"$REPO/scripts/prune_bundle.sh" "$APP"

# ---- 3. sign, inside-out ---------------------------------------------------
# Nested code first; the app bundle last. Everything gets the hardened
# runtime and a secure timestamp, which notarization requires.
echo "==> codesign"
sign() { codesign --force --options runtime --timestamp --sign "$IDENTITY" "$@"; }

# Sparkle ships nested code that arrives signed by the Sparkle project, and
# notarization rejects anything not signed with THIS Developer ID: the two
# XPC services, Updater.app, and the bare Autoupdate executable. They must
# be re-signed innermost-first, before the framework that contains them.
# (Getting this wrong is silent until Apple answers: the first notarization
# attempt failed with 12 issues, all of them here.)
SPARKLE="$APP/Contents/Frameworks/Sparkle.framework"
if [ -d "$SPARKLE" ]; then
    SPV="$SPARKLE/Versions/B"
    for nested in \
        "$SPV/XPCServices/Downloader.xpc" \
        "$SPV/XPCServices/Installer.xpc" \
        "$SPV/Updater.app" \
        "$SPV/Autoupdate"
    do
        [ -e "$nested" ] && sign "$nested"
    done
    sign "$SPV"
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

# ---- 3b. notarize + staple the app itself ---------------------------------
# The app gets its own ticket, stapled, BEFORE it goes into the DMG. The
# DMG's ticket (step 5) covers the app while it is opened from the DMG, but
# the copy in /Applications, and the copy Sparkle installs, carry only what
# is stapled to the bundle: without this step they verify online only, and
# an offline first launch is refused. The 2026-09-22 rewrite of this script
# had dropped the step; the 2.3.3 DMG, built with the lost original, has the
# app stapled (checked 2026-09-25).
if [ "$SKIP_NOTARIZE" -eq 0 ]; then
    echo "==> notarize app (this waits for Apple; usually a few minutes)"
    APP_ZIP="$DIST/qcview-notarize.zip"
    rm -f "$APP_ZIP"
    ditto -c -k --keepParent "$APP" "$APP_ZIP"
    xcrun notarytool submit "$APP_ZIP" --keychain-profile "$PROFILE" --wait
    rm -f "$APP_ZIP"
    echo "==> staple app"
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP"
    spctl --assess --type exec -vv "$APP"
fi

# ---- 4. DMG ----------------------------------------------------------------
echo "==> DMG"
rm -rf "$DIST/stage" "$DMG"
mkdir -p "$DIST/stage"
cp -R "$APP" "$DIST/stage/"   # the staged, signed bundle
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
