#!/usr/bin/env bash
# montage.sh <dir>: per-view rows (1.30 1.42 1.80 2.55) and grouped montages.
set -e
cd "$1"
for v in shakex shakexl shakeu shaked shakew shakeq; do
    convert "${v}_1.30.png" "${v}_1.42.png" "${v}_1.80.png" "${v}_2.55.png" +append -resize 50% "m_$v.png"
done
convert m_shakex.png m_shakexl.png m_shakeu.png -append montage_views_A.png
convert m_shaked.png m_shakew.png m_shakeq.png -append montage_views_B.png
convert only0_1.80.png only1_1.80.png +append -resize 60% montage_only.png
ls montage*
