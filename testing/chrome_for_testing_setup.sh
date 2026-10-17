#!/bin/bash
# Installs the Chrome for Testing build testing/harness.js drives, world-readable, idempotently.
#
# The version is the harness's CHROME_FOR_TESTING_VERSION, read from testing/harness.js so the two cannot
# disagree. The harness runs Chrome as an unprivileged account (its sandbox stays on), which cannot traverse
# root's home where puppeteer installs, so the build goes under /opt with a+rX. The download is the official
# archive from Chrome for Testing's public bucket; TLS verification stays on.
#
# Usage: bash testing/chrome_for_testing_setup.sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
VER=$(sed -n 's/^const CHROME_FOR_TESTING_VERSION = "\([0-9.]*\)";$/\1/p' "$HERE/harness.js")
[ -n "$VER" ] || { echo "REFUSING: testing/harness.js states no CHROME_FOR_TESTING_VERSION" >&2; exit 1; }
DEST="/opt/chrome-for-testing-$VER"
if [ ! -x "$DEST/chrome" ]; then
  TMP=$(mktemp -d)
  curl -sSf -o "$TMP/chrome.zip" "https://storage.googleapis.com/chrome-for-testing-public/$VER/linux64/chrome-linux64.zip"
  unzip -q "$TMP/chrome.zip" -d "$TMP"
  mkdir -p "$DEST"
  cp -r "$TMP/chrome-linux64/." "$DEST/"
  rm -rf "$TMP"
fi
chmod -R a+rX "$DEST"
GOT=$("$DEST/chrome" --version)
case "$GOT" in
  *" $VER"*) echo "$DEST/chrome is $GOT" ;;
  *) echo "REFUSING: $DEST/chrome reports \`$GOT\`, not $VER" >&2; exit 1 ;;
esac
