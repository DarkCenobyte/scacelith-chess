#!/usr/bin/env bash
# Runs the cross-compiled Windows tests under wine, from the repository root like build/scacelith_tests.
# Usage: tools/test_win.sh [filter-substring]   (SCACELITH_BUILD_WIN: the build folder, default build-win)
#
# Wine names the Linux files in the character set of the host locale (LC_ALL, LC_CTYPE, LANG). In
# the POSIX locale (LANG unset, as in many containers) that is ASCII: a file named after "Élodie"
# cannot be created at all (ERROR_FILE_NOT_FOUND), and the saved games' tests fail although Windows
# takes the name. The tests run in a UTF-8 locale when the current one is not.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${SCACELITH_BUILD_WIN:-$ROOT/build-win}"
if [ "$(locale charmap 2>/dev/null)" != "UTF-8" ]; then export LC_ALL=C.UTF-8; fi
export WINEDEBUG=${WINEDEBUG:--all}
cd "$ROOT"
exec wine "$BUILD/scacelith_tests.exe" "$@"
