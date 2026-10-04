#!/usr/bin/env bash
# mk.sh <tool> [out] [extra g++ flags]: builds tools/handshake/<tool>.cpp against $HS_SRC's sources
# and $HS_BUILD's core library into $HS_WORK/bin/<out> (default <tool>). See README.md.
set -e
. "$(dirname "$0")/env.sh"
N="$1"; shift
O="${1:-$N}"; [ $# -gt 0 ] && shift
cd "$HS_SRC"
g++ -std=c++17 -O2 -march=native -w -Isrc -I"$HS_BUILD/generated" -I"$HS_TOOLS" "$@" -o "$HS_WORK/bin/$O" "$HS_TOOLS/$N.cpp" \
    src/character/skeleton.cpp src/character/robot_body.cpp src/character/robot_head.cpp src/character/robot_model.cpp \
    src/character/sdf_mesh.cpp src/character/sdf_volume.cpp src/render/mesh.cpp src/gl/gl46.cpp "$HS_BUILD/libscacelith_core.a" -lpthread
