#!/usr/bin/env bash
# The X11 desktop tests (tests/x11_desktop_tests.cpp: clipboard, input method, keys, focus,
# fullscreen) on an Xvfb of their own: first without a window manager, then with openbox (EWMH
# fullscreen). They change that server's keyboard map, clipboard and focus, never the desktop's.
# Needs Xvfb and openbox (Debian/Ubuntu: apt install xvfb openbox).
# Usage: tools/test_x11.sh [build dir, default build] [filter-substring, default x11_]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
FILTER="${2:-x11_}"
TESTS="$BUILD/scacelith_tests"
[ -x "$TESTS" ] || { echo "no $TESTS (ninja -C $BUILD scacelith_tests)" >&2; exit 1; }

# A display number no server uses.
n=90
while [ -e "/tmp/.X11-unix/X$n" ] || [ -e "/tmp/.X$n-lock" ]; do n=$((n + 1)); done
export DISPLAY=":$n"
Xvfb "$DISPLAY" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
xvfb=$!
wm=
cleanup() {
    [ -n "$wm" ] && kill "$wm" 2>/dev/null || true
    kill "$xvfb" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT
for _ in $(seq 100); do [ -e "/tmp/.X11-unix/X$n" ] && break; sleep 0.05; done
[ -e "/tmp/.X11-unix/X$n" ] || { echo "Xvfb $DISPLAY did not start" >&2; exit 1; }

export SCACELITH_X11_TESTS=1
unset XMODIFIERS
echo "== $DISPLAY without a window manager"
"$TESTS" --require=x11 "$FILTER"
openbox >/dev/null 2>&1 &
wm=$!
echo "== $DISPLAY with openbox"
SCACELITH_X11_WM=1 "$TESTS" --require=x11,x11-wm "$FILTER"
