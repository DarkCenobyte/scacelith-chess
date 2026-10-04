#!/usr/bin/env bash
# P=<fit .txt> ballprobe.sh: CMC ball exposure for a few grip thumb (opposition, CMC) pairs.
. "$(dirname "$0")/env.sh"
for t in "0.3 0.15" "0.8 0.0" "1.2 -0.2" "1.4 0.2" "1.4 -0.4" "1.0 0.4" "1.5 0.6" "0.6 0.6"; do
  set -- $t
  sed "s/^gT0 .*/gT0 $1/; s/^gT1 .*/gT1 $2/" "$P" > "$HS_WORK/ballprobe.txt"
  echo "opp $1 cmc $2: $("$HS_TOOLS/fit.sh" show "$HS_WORK/ballprobe.txt" 2>&1 | grep -o 'CMC ball exposed [0-9]*%\|pen grip .*mm (')"
done
