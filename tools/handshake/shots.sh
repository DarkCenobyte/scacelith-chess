#!/usr/bin/env bash
# shots.sh <outdir> <view,view,...> <time,time,...> [extra args]: anim-viewer screenshots (sharp, 1280x720)
# with $HS_BUILD, one at a time. sharp.ini: no depth of field, no motion blur. Each shot plays the
# 0.5 s before t (--play, 30 frames at 1/60 s), so the temporal filters (GTAO, TAA, SSR) carry the
# motion's history as in the game: a frozen 8-frame shot shows the ambient occlusion unconverged (a
# grey speckle in the crevices). FROZEN=1 takes the old frozen shots.
OUT="$1"; VIEWS="$2"; TIMES="$3"; shift 3
. "$(dirname "$0")/env.sh"
mkdir -p "$OUT"
export DISPLAY=${DISPLAY:-:137}   # an Xvfb display of your own
export SCACELITH_BUILD="$HS_BUILD"
for t in ${TIMES//,/ }; do
  for v in ${VIEWS//,/ }; do
    name="${v}_${t}"
    [ -n "$SUFFIX" ] && name="${name}_$SUFFIX"
    if [ -n "$FROZEN" ]; then
      "$HS_SRC/tools/shot.sh" anim "$OUT/$name.png" 8 1280x720 --robot --view "$v" --time "$t" --ini "$HS_TOOLS/sharp.ini" "$@" > "$OUT/$name.log" 2>&1
    else
      t0=$(awk -v t="$t" 'BEGIN { s = t - 0.5; if (s < 0) s = 0; print s }')
      n=$(awk -v t="$t" -v s="$t0" 'BEGIN { n = int((t - s) * 60 + 0.5); print n < 1 ? 1 : n }')
      "$HS_SRC/tools/shot.sh" anim "$OUT/$name.png" "$n" 1280x720 --robot --view "$v" --time "$t0" --play --ini "$HS_TOOLS/sharp.ini" "$@" > "$OUT/$name.log" 2>&1
    fi
    echo "$name $?"
  done
done
