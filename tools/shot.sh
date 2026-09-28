#!/usr/bin/env bash
# Headless screenshot of a scene with the Linux build (Mesa llvmpipe, OpenGL 4.6 override).
# Usage: tools/shot.sh <scene> <out.png> [frames] [WxH] [extra args...]
# Shaders are read from the working tree (--data-dir), so shader edits need no rebuild.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCENE="$1"; OUT="$2"; FRAMES="${3:-8}"; SIZE="${4:-1280x720}"; shift 4 || true
if ! pgrep -x Xvfb >/dev/null; then (Xvfb :99 -screen 0 1920x1080x24 >/dev/null 2>&1 &); sleep 1; fi
export DISPLAY=${DISPLAY:-:99} MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460
BUILD="${SCACELITH_BUILD:-$ROOT/build}"
"$BUILD/scacelith" --scene "$SCENE" --shot "$OUT" --frames "$FRAMES" --size "$SIZE" --data-dir "$ROOT" "$@"
