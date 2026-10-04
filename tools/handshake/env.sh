# Sourced by the scripts here. HS_SRC: the source tree measured (default: this repository; a git
# worktree with a changed handshake works too), HS_BUILD: its configured build directory (needs
# libscacelith_core.a and generated/), HS_WORK: binaries, fits, grids cache and outputs.
HS_TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HS_SRC=${HS_SRC:-$(cd "$HS_TOOLS/../.." && pwd)}
HS_BUILD=${HS_BUILD:-$HS_SRC/build}
HS_WORK=${HS_WORK:-$HS_BUILD/handshake}
export HS_TOOLS HS_SRC HS_BUILD HS_WORK
mkdir -p "$HS_WORK/bin"
