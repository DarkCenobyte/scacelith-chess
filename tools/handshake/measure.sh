#!/usr/bin/env bash
# measure.sh <tag>: builds pen against $HS_SRC as it is, runs the full-task exact-mesh check and the cut cases.
set -e
. "$(dirname "$0")/env.sh"
tag=$1; out=$HS_WORK/$tag; mkdir -p $out
"$HS_TOOLS/mk.sh" pen pen_$tag
for m in rr rl lr rlpen; do PHASES=0.60,0.78,0.92,1.80,1.96,2.10 "$HS_WORK/bin/pen_$tag" $m exact > $out/$m.txt 2>&1 & done; wait
for m in rr rl lr rlpen; do grep -A12 '^== ' $out/$m.txt; done > $out/summary.txt
( for m in cutrr cutrl cutlr; do MODES=$m PEN=pen_$tag SAVE=$out CUTS="${CUTS:-0.70 0.90 1.2 1.85 2.0}" "$HS_TOOLS/cutscan.sh" > $out/cut_$m.txt & done; wait )
cat $out/cut_*.txt > $out/cutscan.txt
