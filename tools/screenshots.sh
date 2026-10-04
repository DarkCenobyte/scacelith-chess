#!/bin/sh
# Turns a screenshot into the WebP files the site uses, with ImageMagick (`magick`, or `convert`
# for ImageMagick 6). No build is needed afterwards: commit the files in assets/img/.
#
#   tools/screenshots.sh title-screen    ~/Pictures/menu.png
#   tools/screenshots.sh the-grand-hall  ~/Pictures/hall.png
#   tools/screenshots.sh hero            ~/Pictures/scene-without-text.png
#
# Gallery names (home page, in this order): title-screen, the-grand-hall, at-the-board,
# the-opponent, the-scoresheet. Each gets assets/img/<name>.webp (the full picture, at most
# 2560 px wide, shown in the viewer) and assets/img/<name>-1000.webp (the thumbnail, 1000 px
# wide). The thumbnails are shown in 16:9 frames: other proportions are cropped at the edges.
#
# "hero" is the large picture at the top of the home page, also the background of the
# language page and of the inner pages' headers: a scene without interface text, ideally about
# 2:1 and at least 1600 px wide. It gets assets/img/hero.webp (at most 1920 px wide),
# assets/img/hero-960.webp and assets/img/og-scacelith.jpg (1200 x 630, the picture that link
# previews show).
#
# The captions and descriptions (alt text) of the gallery pictures are in i18n/<lang>.json
# under "gallery"; change them there if a new picture shows something else, then run
# python3 tools/build.py.
set -eu

if [ $# -ne 2 ]; then
  sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
fi
name=$1
src=$2
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/assets/img"

if command -v magick >/dev/null 2>&1; then IM=magick; elif command -v convert >/dev/null 2>&1; then IM=convert; else
  echo "ImageMagick is needed (magick or convert)" >&2
  exit 1
fi
[ -f "$src" ] || { echo "no such file: $src" >&2; exit 1; }

webp() { # source width quality target
  "$IM" "$1" -auto-orient -strip -resize "$2x>" -quality "$3" -define webp:method=6 "$4"
}

case "$name" in
  title-screen|the-grand-hall|at-the-board|the-opponent|the-scoresheet)
    webp "$src" 2560 82 "$out/$name.webp"
    webp "$src" 1000 80 "$out/$name-1000.webp"
    ;;
  hero)
    webp "$src" 1920 82 "$out/hero.webp"
    webp "$src" 960 80 "$out/hero-960.webp"
    "$IM" "$src" -auto-orient -strip -resize 1200x630^ -gravity center -extent 1200x630 -quality 85 "$out/og-scacelith.jpg"
    ;;
  *)
    echo "unknown name: $name (title-screen, the-grand-hall, at-the-board, the-opponent, the-scoresheet or hero)" >&2
    exit 2
    ;;
esac
ls -l "$out" | grep -E " ($name|og-scacelith)" || true
