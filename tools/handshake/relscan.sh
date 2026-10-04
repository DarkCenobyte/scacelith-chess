#!/usr/bin/env bash
# relscan.sh: release timing scan with an env-tunable pen build (pen_tune: a pen built against a
# tree whose planHandshake reads SH_* timing overrides; not in the repository), grid SDFs (fast).
. "$(dirname "$0")/env.sh"
for tp in ${TPS:-1.90}; do
for to in ${TOS:-2.08}; do
for tw in ${TWS:-2.18}; do
for vk in ${VKS:-1.0}; do
for w in ${WS:-0}; do
for f in ${FS:-1}; do
for he in ${HES:-0}; do
    out=$(SH_OE=${OE:-0.96} SH_TP=$tp SH_TO=$to SH_TW=$tw SH_VK=$vk SH_W=$w SH_F=$f SH_HE=$he PHASES=0.60,0.78,0.92,$tp,$to,$tw "$HS_WORK/bin/pen_tune" ${MODE:-rr} $EXTRA 2>&1)
    sp=$(echo "$out" | grep "hand speed" | sed 's/.*peak on the way back \([0-9.]*\).*max acceleration in the release \([0-9.]*\).*/back \1 acc \2/')
    pw=$(echo "$out" | grep "phase 6withdraw\|phase 7retract\|phase 5open" | awk '{print $2, $4}' | tr '\n' ' ')
    pops=$(echo "$out" | grep -o "pops over 1800 deg/s: [0-9]*")
    echo "tp $tp to $to tw $tw vk $vk w $w f $f he $he: $sp | $pw | $pops"
    [ -n "$PROFILE" ] && echo "$out" | grep "^   speed u=[2]\."
done; done; done; done; done; done; done
