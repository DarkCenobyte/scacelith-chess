#!/usr/bin/env bash
# cutscan.sh: the handshake cut short at several instants (CUTS), with the pen build $PEN
# (default pen; MODES = cutrr cutrl cutlr). SAVE=<dir> keeps each full output.
. "$(dirname "$0")/env.sh"
P=${PEN:-pen}
for m in ${MODES:-cutrr cutrl}; do
  for c in ${CUTS:-0.65 0.85 1.2 1.5 1.85 2.0}; do
    out=$(CUTAT=$c PHASES=0.60,0.78,0.92,1.80,1.96,2.10 "$HS_WORK/bin/$P" $m ${EXACT:-exact} 2>&1)
    [ -n "$SAVE" ] && echo "$out" > "$SAVE/${m}_$c.txt"
    w=$(echo "$out" | grep '^== ' | sed 's/.*worst interpenetration //')
    bs=$(echo "$out" | grep "cut: B hand speed" | tail -1 | sed 's/.*speed //')
    ws=$(echo "$out" | grep "cut: W hand speed" | tail -1 | sed 's/.*speed //')
    pops=$(echo "$out" | grep -o 'pops over 1800 deg/s: [0-9]*' | sed 's/.*: //')
    ang=$(echo "$out" | grep -o 'max angular speed [0-9]* deg/s ([^)]*)' | sed 's/max angular speed //')
    echo "$m cut $c: $w | B max $bs, W max $ws | pops $pops, max $ang"
  done
done
