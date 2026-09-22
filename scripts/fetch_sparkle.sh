#!/bin/bash
# fetch_sparkle.sh — download the pinned Sparkle release into external/Sparkle.
#
# Sparkle is not a submodule: the binary release carries the framework, the
# XPC services and the tools (generate_keys, sign_update, generate_appcast)
# that the release flow uses. The SHA-256 below is checked before anything is
# unpacked — an update framework fetched over the network without one is a
# supply-chain hole.
#
# Usage: scripts/fetch_sparkle.sh [--force]
set -euo pipefail

VERSION="2.9.2"
SHA256="1cb340cbbef04c6c0d162078610c25e2221031d794a3449d89f2f56f4df77c95"   # dependencies.md §Sparkle
URL="https://github.com/sparkle-project/Sparkle/releases/download/$VERSION/Sparkle-$VERSION.tar.xz"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO/external/Sparkle"

if [ -d "$DEST/Sparkle.framework" ] && [ "${1:-}" != "--force" ]; then
    echo "Sparkle already in $DEST (use --force to refetch)"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "==> downloading Sparkle $VERSION"
curl -fsSL -o "$TMP/sparkle.tar.xz" "$URL"

GOT="$(shasum -a 256 "$TMP/sparkle.tar.xz" | awk '{print $1}')"
if [ "$GOT" != "$SHA256" ]; then
    echo "SHA-256 mismatch for Sparkle $VERSION" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $GOT" >&2
    echo "Do not use this download. Check the release page; if Sparkle" >&2
    echo "re-cut the tarball, verify by hand and update the pin here and" >&2
    echo "in dependencies.md." >&2
    exit 1
fi

echo "==> unpacking"
mkdir -p "$DEST"
tar xJf "$TMP/sparkle.tar.xz" -C "$DEST"
echo "==> Sparkle $VERSION in $DEST"
ls "$DEST/bin" 2>/dev/null | sed 's/^/    /'
