#!/bin/sh
# Adds Scacelith to the current user's applications menu, or removes it (--uninstall): the
# executable in ~/.local/bin, the launcher in ~/.local/share/applications and the icons in
# ~/.local/share/icons/hicolor. Nothing outside the home folder, no root.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin="$HOME/.local/bin"
data="${XDG_DATA_HOME:-$HOME/.local/share}"
sizes="16 24 32 48 64 96 128 256 512"
if [ "${1-}" = --uninstall ]; then
    rm -f "$bin/scacelith" "$data/applications/scacelith.desktop"
    for s in $sizes; do rm -f "$data/icons/hicolor/${s}x$s/apps/scacelith.png"; done
    echo "Scacelith removed from $bin and $data."
    exit 0
fi
install -D -m 755 "$here/scacelith" "$bin/scacelith"
for s in $sizes; do
    install -D -m 644 "$here/share/icons/hicolor/${s}x$s/apps/scacelith.png" "$data/icons/hicolor/${s}x$s/apps/scacelith.png"
done
# The launcher runs the executable by its full path: ~/.local/bin is not on every session's PATH.
# Desktop entry quoting: \ " ` $ are escaped inside the quotes, then every \ is doubled (the
# string escape of the file), and % is doubled.
exe=$(printf '%s' "$bin/scacelith" | sed -e 's/[\\"`$]/\\&/g' -e 's/\\/\\\\/g' -e 's/%/%%/g')
mkdir -p "$data/applications"
EXE=$exe awk '/^Exec=/ { print "Exec=\"" ENVIRON["EXE"] "\""; next } { print }' \
    "$here/share/applications/scacelith.desktop" > "$data/applications/scacelith.desktop"
echo "Scacelith installed: $bin/scacelith, with its launcher in the applications menu."
