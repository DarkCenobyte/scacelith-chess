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
# time (libsecret-1.so.0) when present. This looks for the library of the executable's architecture
# (x86-64 or aarch64) in ldconfig's cache, else in the usual library folders (ldconfig is not on
# every user's PATH, nor everywhere): a hint only, the game makes its own check when it starts.
# The ELF machine of a file (bytes 18-19, little-endian): 3e00 for x86-64, b700 for aarch64.
elf_machine() { od -An -tx1 -j18 -N2 "$1" 2>/dev/null | tr -d ' \n'; }
machine=$(elf_machine "$here/scacelith")
case "$machine" in
    3e00) tag=x86-64 triplet=x86_64-linux-gnu ;;
    b700) tag=AArch64 triplet=aarch64-linux-gnu ;;
    *) tag= triplet= ;;
esac
libsecret_found() {
    [ -n "$tag" ] || return 2
    for ldconfig in "$(command -v ldconfig || true)" /sbin/ldconfig /usr/sbin/ldconfig; do
        if [ -n "$ldconfig" ] && [ -x "$ldconfig" ]; then
            # "libsecret-1.so.0 (libc6,AArch64) => ...", or "(libc6,x86-64, OS ABI: Linux 3.2.0)".
            "$ldconfig" -p 2>/dev/null | grep -q "libsecret-1\.so\.0 (libc6,$tag[,)]" && return 0
            break
        fi
    done
    # Debian multiarch, then Fedora and openSUSE (lib64), then Arch: a file of the same machine.
    for dir in "/usr/lib/$triplet" "/lib/$triplet" /usr/lib64 /lib64 /usr/lib /lib /usr/local/lib64 /usr/local/lib; do
        [ -e "$dir/libsecret-1.so.0" ] && [ "$(elf_machine "$dir/libsecret-1.so.0")" = "$machine" ] && return 0
    done
    return 1
}
found=0
libsecret_found || found=$?
if [ "$found" = 1 ]; then
    echo "Note: libsecret-1.so.0 (for $tag) was not found in ldconfig's cache or the usual library folders. If the game cannot load it either, it keeps the saved logins in a file only you can read in ${XDG_CONFIG_HOME:-$HOME/.config}/scacelith/ instead of the desktop keyring, or only until it closes if you turn off \"Remember my sign-in when the system keyring is unavailable\" in Options > Online server; it tells you so when you sign in. To use the keyring, install libsecret-1-0 on Debian/Ubuntu, libsecret on Fedora/Arch."
fi
