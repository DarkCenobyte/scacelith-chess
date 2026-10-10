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
# The saved logins' tokens go to the desktop keyring through libsecret, which the game loads at run
# time when present: a notice, not an error, when it cannot be found (ldconfig's cache, else the
# usual library folders: ldconfig is not on every user's PATH, nor everywhere).
libsecret_found() {
    for ldconfig in "$(command -v ldconfig || true)" /sbin/ldconfig /usr/sbin/ldconfig; do
        if [ -n "$ldconfig" ] && [ -x "$ldconfig" ]; then
            "$ldconfig" -p 2>/dev/null | grep -q 'libsecret-1\.so\.0 (libc6,x86-64)' && return 0
            break
        fi
    done
    for dir in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /lib64 /usr/lib /lib /usr/local/lib; do
        [ -e "$dir/libsecret-1.so.0" ] && return 0
    done
    return 1
}
if ! libsecret_found; then
    echo "Note: libsecret-1.so.0 not found: the saved logins will be kept in ${XDG_CONFIG_HOME:-$HOME/.config}/scacelith/ instead of the desktop keyring (install libsecret-1-0 on Debian/Ubuntu, libsecret on Fedora/Arch)."
fi
