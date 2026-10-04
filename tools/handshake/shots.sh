#!/usr/bin/env bash
# shots.sh <outdir> <view,view,...> <time,time,...> [extra args]: anim-viewer screenshots (sharp, 1280x720)
# with $HS_BUILD, one at a time. sharp.ini: no depth of field, no motion blur.
OUT="$1"; VIEWS="$2"; TIMES="$3"; shift 3
. "$(dirname "$0")/env.sh"
mkdir -p "$OUT"
export DISPLAY=${DISPLAY:-:137}   # an Xvfb display of your own
export SCACELITH_BUILD="$HS_BUILD"
for t in ${TIMES//,/ }; do
  for v in ${VIEWS//,/ }; do
    name="${v}_${t}"
    [ -n "$SUFFIX" ] && name="${name}_$SUFFIX"
    "$HS_SRC/tools/shot.sh" anim "$OUT/$name.png" 8 1280x720 --robot --view "$v" --time "$t" --ini "$HS_TOOLS/sharp.ini" "$@" > "$OUT/$name.log" 2>&1
    echo "$name $?"
  done
done
