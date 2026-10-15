#!/usr/bin/env bash
# The AppImage of the Linux x86-64 release: the game's executable with its launcher, icons and
# licences in one file that runs without unpacking or installing anything. Nothing else is bundled:
# like the .tar.gz archive, it uses the system's glibc, OpenSSL, X11, OpenGL, ALSA and libsecret
# (docs/PORTS.md says why). Made with appimagetool and the AppImage type 2 runtime, pinned releases
# checked by their SHA-256 (updated by hand: Dependabot does not see them), then checked: the
# AppImage extracts to the files put in, and runs (--list-scenes, in extract-and-run mode, so
# without FUSE) with its log in the user's folder, not in its own.
#
# Usage: tools/linux/make-appimage.sh <scacelith executable> [<output.AppImage>]
# The default output is build/Scacelith-<version>-linux-x86_64.AppImage, the name of the release
# file. Needs curl, cmake (for the version) and desktop-file-validate (desktop-file-utils), which
# appimagetool runs on the launcher.
set -euo pipefail

APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
die() { echo "make-appimage: $*" >&2; exit 1; }
step() { echo "== $*"; }

[[ $# -ge 1 && $# -le 2 ]] || die "usage: $0 <scacelith executable> [<output.AppImage>]"
EXE=$1
[[ -f "$EXE" && -x "$EXE" ]] || die "$EXE is not an executable"
# The ELF machine of the file (bytes 18-19, little-endian): 3e00 for x86-64.
[[ "$(od -An -tx1 -j18 -N2 "$EXE" | tr -d ' \n')" == 3e00 ]] || die "$EXE is not an x86-64 executable"
[[ "$(uname -m)" == x86_64 ]] || die "appimagetool is pinned for x86-64 hosts only"
command -v desktop-file-validate > /dev/null || die "desktop-file-validate is missing (desktop-file-utils)"
# appimagetool writes $VERSION into the root launcher (X-AppImage-Version).
VERSION=$(cmake -P "$ROOT/cmake/version.cmake")
export VERSION
OUT=${2:-$ROOT/build/Scacelith-$VERSION-linux-x86_64.AppImage}
[[ "$OUT" == *.AppImage ]] || die "the output must be an .AppImage file, not $OUT"
mkdir -p "$(dirname "$OUT")"
OUT=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

download_checked() { # <url> <sha256> <file>
    local i
    for i in 1 2 3; do
        curl -fsSL -o "$3" "$1" && break
        [[ $i == 3 ]] && die "could not download $1"
        sleep $((i * 5))
    done
    echo "$2  $3" | sha256sum -c --quiet - ||
        die "$1 does not have the pinned SHA-256 $2 (got $(sha256sum "$3" | cut -d' ' -f1))"
    chmod +x "$3"
    echo "$(basename "$3"): SHA-256 $2"
}

step "appimagetool and the runtime"
download_checked "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA256" "$TMP/appimagetool"
download_checked "$RUNTIME_URL" "$RUNTIME_SHA256" "$TMP/runtime"

# AppRun is the executable itself: nothing bundled, so no environment to set up (and no library
# path handed on to the programs the game starts: xdg-open, the browser, the file manager). The
# launcher and the 256 px icon stand at the root too, where AppImage tools look for them.
step "AppDir"
APPDIR=$TMP/AppDir
DOC=$APPDIR/usr/share/doc/scacelith
install -D -m 755 "$EXE" "$APPDIR/usr/bin/scacelith"
ln -s usr/bin/scacelith "$APPDIR/AppRun"
desktop-file-validate "$ROOT/res/linux/scacelith.desktop"
install -D -m 644 "$ROOT/res/linux/scacelith.desktop" "$APPDIR/usr/share/applications/scacelith.desktop"
install -m 644 "$ROOT/res/linux/scacelith.desktop" "$APPDIR/scacelith.desktop"
for size in 16 24 32 48 64 96 128 256 512; do
    install -D -m 644 "$ROOT/res/icons/png/scacelith-$size.png" \
        "$APPDIR/usr/share/icons/hicolor/${size}x${size}/apps/scacelith.png"
done
install -m 644 "$ROOT/res/icons/png/scacelith-256.png" "$APPDIR/scacelith.png"
# The licence and the licence texts of what the game embeds, as in the release archives, and those
# of the runtime at the start of the AppImage file.
install -D -m 644 "$ROOT/LICENSE" "$DOC/LICENSE"
mkdir -p "$DOC/licences/fonts" "$DOC/licences/AppImage-runtime"
cp "$ROOT"/assets/fonts/*.txt "$ROOT"/assets/fonts/hand/*.txt "$DOC/licences/fonts/"
cp "$ROOT/third_party/stockfish/AUTHORS" "$DOC/licences/Stockfish-AUTHORS.txt"
cp "$ROOT/third_party/qrcodegen/LICENSE" "$DOC/licences/qrcodegen-MIT.txt"
cp "$ROOT/assets/licences/lichess-chess-openings-CC0.txt" "$DOC/licences/"
cp "$ROOT"/res/linux/appimage-runtime/*.txt "$DOC/licences/AppImage-runtime/"
chmod -R u=rwX,go=rX "$DOC"

# The files' times are the commit's (when built from a git checkout), as in the .tar.gz archive:
# the same commit gives the same AppImage.
step "AppImage $(basename "$OUT")"
if epoch=$(git -C "$ROOT" log -1 --format=%ct 2> /dev/null); then
    export SOURCE_DATE_EPOCH=$epoch
    find "$APPDIR" -exec touch -h -d "@$epoch" {} +
fi
rm -f "$OUT"
# appimagetool is itself an AppImage: extract-and-run needs no FUSE. No update information and no
# AppStream metadata (docs/PORTS.md).
(cd "$TMP" && ARCH=x86_64 TMPDIR=$TMP ./appimagetool --appimage-extract-and-run --no-appstream \
    --runtime-file "$TMP/runtime" "$APPDIR" "$OUT") || die "appimagetool failed"
[[ -x "$OUT" ]] || die "appimagetool made no executable $OUT"

step "Checks"
# The runtime is the pinned one (appimagetool writes only the file system's MD5 digest into it, in
# its .digest_md5 section, empty in the pinned file), and the file system after it holds exactly
# the files put in.
offset=$("$OUT" --appimage-offset)
read -r md5_at md5_size < <(readelf -S --wide "$TMP/runtime" |
    sed -n 's/.*\.digest_md5 *PROGBITS *[0-9a-f]* \([0-9a-f]*\) \([0-9a-f]*\) .*/\1 \2/p')
[[ -n "${md5_size:-}" ]] || die "the pinned runtime has no .digest_md5 section"
head -c "$offset" "$OUT" > "$TMP/runtime-out"
dd if=/dev/zero of="$TMP/runtime-out" bs=1 seek=$((16#$md5_at)) count=$((16#$md5_size)) conv=notrunc status=none
[[ "$(sha256sum < "$TMP/runtime-out" | cut -d' ' -f1)" == "$RUNTIME_SHA256" ]] ||
    die "the runtime at the start of $OUT is not the pinned one"
(cd "$TMP" && "$OUT" --appimage-extract > /dev/null) || die "$OUT does not extract"
[[ "$(readlink "$APPDIR/.DirIcon")" == scacelith.png ]] || die "appimagetool made no .DirIcon link to the icon"
diff -r --no-dereference "$APPDIR" "$TMP/squashfs-root" || die "$OUT does not hold the files of the AppDir"
# A run in extract-and-run mode (no FUSE needed): it prints the scenes and leaves, and its log is
# in the user's folder, not in the AppImage's (src/net/net_sys.cpp). The runtime of 20251108 does
# not always delete the copy it extracted: TMPDIR keeps it in $TMP, which goes at the end.
run=$TMP/run
mkdir -p "$run/home" "$run/tmp"
if ! env -u XDG_CONFIG_HOME -u XDG_DATA_HOME HOME="$run/home" TMPDIR="$run/tmp" APPIMAGE_EXTRACT_AND_RUN=1 \
        "$OUT" --list-scenes > "$run/scenes.txt" 2> "$run/stderr.txt"; then
    cat "$run/stderr.txt"
    die "$OUT --list-scenes failed"
fi
grep -q '^game ' "$run/scenes.txt" || { cat "$run/scenes.txt" "$run/stderr.txt"; die "$OUT did not list its scenes"; }
log=$run/home/.config/scacelith/scacelith.log
grep -q "Scacelith $VERSION starting" "$log" 2> /dev/null || die "$OUT left no log in $log"

ls -l "$OUT"
sha256sum "$OUT"
