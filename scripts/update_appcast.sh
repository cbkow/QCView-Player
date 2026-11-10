#!/bin/bash
# update_appcast.sh — add a release to docs/appcast.xml.
#
# docs/ IS the qcview.app site (GitHub Pages, CNAME in docs/CNAME), so the
# appcast is published by committing this file and pushing main. Sparkle
# fetches https://qcview.app/appcast.xml and verifies each download against
# SUPublicEDKey in the app's Info.plist, so the signature below must come
# from the matching private key — the one in the login keychain, which
# sign_update uses by default.
#
# Run it AFTER the DMG is attached to the GitHub release, because the
# enclosure URL points at the release asset. The DMG argument is the local
# file: its bytes are what gets signed and measured.
#
# sign_update is Sparkle's tool. QCView 3 is built in the minplatform repository, whose tools/sparkle/fetch.sh puts it in
# external/sparkle/bin; this script looks for a minplatform checkout beside this one, or takes the path in SIGN_UPDATE.
#
# Usage: scripts/update_appcast.sh <version> <path/to/QCView-MacOS.dmg>
#        SIGN_UPDATE=/path/to/sign_update scripts/update_appcast.sh 3.0.0 QCView-MacOS.dmg
set -euo pipefail

VERSION="${1:-}"
DMG="${2:-}"
[ -n "$VERSION" ] && [ -f "$DMG" ] || {
    echo "usage: $0 <version> <path/to/QCView-MacOS.dmg>" >&2; exit 1; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPCAST="$REPO/docs/appcast.xml"
SIGN="${SIGN_UPDATE:-$REPO/../minplatform/external/sparkle/bin/sign_update}"
MIN_OS="13.0"                    # keep in step with the app's minimum macOS (minplatform: packaging/macos)
[ -f "$APPCAST" ] || { echo "no appcast at $APPCAST" >&2; exit 1; }
[ -x "$SIGN" ] || { echo "no sign_update at $SIGN (minplatform: tools/sparkle/fetch.sh, or set SIGN_UPDATE)" >&2; exit 1; }

grep -q "<sparkle:version>$VERSION</sparkle:version>" "$APPCAST" && {
    echo "appcast already has $VERSION — bump the version or edit by hand" >&2
    exit 1; }

# sign_update prints: sparkle:edSignature="…" length="…"
SIGLINE="$($SIGN "$DMG")"
SIGNATURE="$(echo "$SIGLINE" | sed -E 's/.*sparkle:edSignature="([^"]*)".*/\1/')"
LENGTH="$(echo "$SIGLINE" | sed -E 's/.*length="([^"]*)".*/\1/')"
[ -n "$SIGNATURE" ] && [ -n "$LENGTH" ] || {
    echo "sign_update gave nothing usable: $SIGLINE" >&2; exit 1; }

PUBDATE="$(LC_ALL=C date '+%a, %d %b %Y %H:%M:%S %z')"
URL="https://github.com/cbkow/QCView-Player/releases/download/v$VERSION/$(basename "$DMG")"

ITEM=$(cat <<XML
    <item>
      <title>Version $VERSION</title>
      <link>https://qcview.app/installation/</link>
      <sparkle:version>$VERSION</sparkle:version>
      <sparkle:shortVersionString>$VERSION</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>$MIN_OS</sparkle:minimumSystemVersion>
      <description><![CDATA[
        <ul><li>See the release notes at https://github.com/cbkow/QCView-Player/releases/tag/v$VERSION</li></ul>
      ]]></description>
      <pubDate>$PUBDATE</pubDate>
      <enclosure
        url="$URL"
        sparkle:edSignature="$SIGNATURE"
        length="$LENGTH"
        type="application/octet-stream" />
    </item>
XML
)

# Newest first: insert above the first existing <item>.
python3 - "$APPCAST" "$ITEM" <<'PY'
import sys
path, item = sys.argv[1], sys.argv[2]
xml = open(path).read()
at = xml.index("    <item>")
open(path, "w").write(xml[:at] + item + "\n" + xml[at:])
PY

echo "==> added $VERSION to docs/appcast.xml"
echo "    url    $URL"
echo "    length $LENGTH"
echo
echo "Check the enclosure URL resolves (the asset must already be attached):"
echo "    curl -sIL '$URL' | head -1"
echo "Then publish — docs/ is the Pages site:"
echo "    git add docs/appcast.xml && git commit -m 'appcast: publish $VERSION' && git push"
echo "If the Pages deploy stalls in 'queued', push an empty commit to nudge it."
