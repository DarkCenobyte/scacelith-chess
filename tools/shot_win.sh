#!/usr/bin/env bash
# Same as shot.sh but runs the cross-compiled Windows exe under wine (end-to-end check).
# Usage: tools/shot_win.sh <scene> <out.png> [frames] [WxH] [extra args...]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCENE="$1"; OUT="$(realpath -m "$2")"; FRAMES="${3:-8}"; SIZE="${4:-1280x720}"; shift 4 || true
if ! pgrep -x Xvfb >/dev/null; then (Xvfb :99 -screen 0 1920x1080x24 >/dev/null 2>&1 &); sleep 1; fi
export DISPLAY=${DISPLAY:-:99} MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460 WINEDEBUG=-all
export WINEPREFIX=${WINEPREFIX:-/tmp/scacelith-wineprefix}
# Wine names the Linux files in the host locale's character set: outside UTF-8, a saved game named
# after "Élodie" cannot be created (see tools/test_win.sh).
if [ "$(locale charmap 2>/dev/null)" != "UTF-8" ]; then export LC_ALL=C.UTF-8; fi
BUILD="${SCACELITH_BUILD_WIN:-$ROOT/build-win}"
wine "$BUILD/Scacelith.exe" --scene "$SCENE" --shot "Z:${OUT//\//\\}" --frames "$FRAMES" --size "$SIZE" "$@"
