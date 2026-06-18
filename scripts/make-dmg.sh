#!/usr/bin/env bash
#
# Build KeyParty.dmg — a branded, drag-to-Applications disk image.
#
# This is the single source of truth shared by CI (.github/workflows/release.yml)
# and local testing, so the install window you see locally is exactly what ships.
#
# Usage:
#   scripts/make-dmg.sh [path/to/App.app] [output.dmg]
#       Build the .dmg. The .app is auto-detected from zig-out/package when
#       omitted (prefers a ReleaseFast build over Debug). Output defaults to
#       KeyParty.dmg in the repo root. Run locally, it opens the image so you
#       can eyeball the window; CI skips that.
#
#   scripts/make-dmg.sh background
#       Re-render assets/dmg-background{,@2x}.png from the .svg after editing
#       the artwork (needs rsvg-convert: brew install librsvg).
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VOLNAME="Key Party"
APP_ON_VOLUME="Key Party.app"
BG_SVG="assets/dmg-background.svg"
BG_1X="assets/dmg-background.png"
BG_2X="assets/dmg-background@2x.png"

render_background() {
  command -v rsvg-convert >/dev/null \
    || { echo "error: rsvg-convert not found (brew install librsvg)" >&2; exit 1; }
  rsvg-convert -w 660  -h 400 "$BG_SVG" -o "$BG_1X"
  rsvg-convert -w 1320 -h 800 "$BG_SVG" -o "$BG_2X"
  echo "rendered $BG_1X and $BG_2X"
}

# Subcommand: just re-render the artwork and stop.
if [ "${1:-}" = "background" ]; then
  render_background
  exit 0
fi

APP="${1:-}"
OUT="${2:-KeyParty.dmg}"

# Locate the .app to package.
if [ -z "$APP" ]; then
  APP="$(ls -d zig-out/package/*ReleaseFast*.app 2>/dev/null | head -n1 || true)"
  [ -z "$APP" ] && APP="$(ls -d zig-out/package/*.app 2>/dev/null | head -n1 || true)"
fi
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
  echo "error: no .app found — pass one explicitly, or build with" >&2
  echo "       'zig build -Dpackage-target=macos' first." >&2
  exit 1
fi
echo "packaging: $APP"

# create-dmg is the de-facto tool for this; install it on demand.
if ! command -v create-dmg >/dev/null; then
  command -v brew >/dev/null \
    || { echo "error: create-dmg not found and no brew to install it (brew install create-dmg)" >&2; exit 1; }
  echo "installing create-dmg via Homebrew..."
  brew install create-dmg
fi

# Backgrounds are committed to the repo; render only if they are missing.
{ [ -f "$BG_1X" ] && [ -f "$BG_2X" ]; } || render_background

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Combine 1x + 2x into a hi-DPI TIFF: crisp on Retina, and correctly sized
# (point dimensions stay 660x400) on every display.
BG_TIFF="$WORK/background.tiff"
tiffutil -cathidpicheck "$BG_1X" "$BG_2X" -out "$BG_TIFF" >/dev/null

# Stage the app under its display name so the icon on the volume always reads
# "Key Party", whatever the build's bundle filename happens to be. ditto keeps
# the code signature and extended attributes intact.
ditto "$APP" "$WORK/$APP_ON_VOLUME"

# A leftover mount from a previous local run would block create-dmg.
hdiutil detach "/Volumes/$VOLNAME" >/dev/null 2>&1 || true
rm -f "$OUT"

build_dmg() {
  create-dmg \
    --volname "$VOLNAME" \
    --volicon assets/icon.icns \
    --background "$BG_TIFF" \
    --window-pos 200 120 \
    --window-size 660 400 \
    --icon-size 128 \
    --icon "$APP_ON_VOLUME" 180 185 \
    --app-drop-link 480 185 \
    --hide-extension "$APP_ON_VOLUME" \
    --no-internet-enable \
    "$OUT" "$WORK/$APP_ON_VOLUME"
}

# create-dmg drives Finder via AppleScript, which occasionally flakes on CI;
# a couple of retries make the build reliable.
ok=0
for attempt in 1 2 3; do
  if build_dmg; then ok=1; break; fi
  echo "create-dmg attempt $attempt failed; retrying in 5s..." >&2
  hdiutil detach "/Volumes/$VOLNAME" >/dev/null 2>&1 || true
  rm -f "$OUT"
  sleep 5
done
[ "$ok" = 1 ] || { echo "error: create-dmg failed after 3 attempts" >&2; exit 1; }

echo "built $OUT"

# Local convenience: mount it so you can see the install window. CI sets $CI.
if [ -z "${CI:-}" ]; then
  echo "opening $OUT — eject the volume when you're done looking."
  open "$OUT"
fi
