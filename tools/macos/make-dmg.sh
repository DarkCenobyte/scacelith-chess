#!/usr/bin/env bash
# The disk image of the macOS release: Scacelith.app and a link to /Applications in a Finder
# window with the background of res/macos/ (the arrow and "Drag Scacelith to Applications"),
# made with dmgbuild (pinned, with its hashes, in tools/macos/requirements-dmgbuild.txt; the
# layout is in tools/macos/dmg-settings.py), then checked: the image verifies, mounts, holds the
# app with a valid signature, the link and the background.
#
# Usage: tools/macos/make-dmg.sh <Scacelith.app> [<output.dmg>]
# The default output is build-macos/Scacelith-<version>-macos-arm64-experimental.dmg, the name of
# the release file; the volume is named "Scacelith <version>".
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=tools/macos/lib.sh
source "$ROOT/tools/macos/lib.sh"

[[ $# -ge 1 && $# -le 2 ]] || die "usage: $0 <Scacelith.app> [<output.dmg>]"
[[ -d "$1/Contents/MacOS" ]] || die "$1 is not an app bundle (tools/macos/make-app.sh makes it)"
APP=$(cd "$1" && pwd)
require_apple_silicon
read_version "$ROOT"
OUT=${2:-$ROOT/build-macos/Scacelith-$VERSION-macos-arm64-experimental.dmg}
[[ "$OUT" == *.dmg ]] || die "the output must be a .dmg file, not $OUT"
mkdir -p "$(dirname "$OUT")"
OUT=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
BACKGROUND=$ROOT/res/macos/dmg-background.png
[[ -f "$BACKGROUND" && -f "${BACKGROUND%.png}@2x.png" ]] || die "the background images are missing from res/macos/"

TMP=$(mktemp -d)
cleanup() {
    [[ -n "${MOUNT:-}" ]] && hdiutil detach -quiet -force "$MOUNT" 2> /dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

step "dmgbuild"
make_venv "$TMP/venv" "$ROOT/tools/macos/requirements-dmgbuild.txt"
"$TMP/venv/bin/python" -m pip list --format=freeze 2> /dev/null | grep -Ei '^(dmgbuild|ds.store|mac.alias)=' || true

step "Disk image $(basename "$OUT")"
rm -f "$OUT"
"$TMP/venv/bin/dmgbuild" -s "$ROOT/tools/macos/dmg-settings.py" \
    -D app="$APP" -D background="$BACKGROUND" -D icon="$APP/Contents/Resources/Scacelith.icns" \
    "Scacelith $VERSION" "$OUT" || die "dmgbuild failed"

step "Checks"
hdiutil verify "$OUT" || die "$OUT does not verify"
hdiutil imageinfo "$OUT" | grep -E '^(Format|Format Description|Size Information|Checksum Type)' || true
MOUNT=$TMP/mount
mkdir -p "$MOUNT"
hdiutil attach -readonly -nobrowse -noautoopen -mountpoint "$MOUNT" "$OUT" > /dev/null || die "$OUT does not mount"
ls -la "$MOUNT"
[[ -d "$MOUNT/Scacelith.app" ]] || die "Scacelith.app is not in the image"
[[ "$(readlink "$MOUNT/Applications")" == /Applications ]] || die "the Applications link is missing"
compgen -G "$MOUNT/.background.*" > /dev/null || die "the background is not in the image"
[[ -f "$MOUNT/.DS_Store" ]] || die "the window layout (.DS_Store) is not in the image"
codesign --verify --deep --strict "$MOUNT/Scacelith.app" || die "the app in the image does not verify"
hdiutil detach -quiet "$MOUNT" || die "could not detach $MOUNT"
MOUNT=
step_end
ls -l "$OUT"
shasum -a 256 "$OUT"
