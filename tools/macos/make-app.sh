#!/usr/bin/env bash
# Assembles Scacelith.app from a macOS build of the game and the dependency prefix of
# tools/macos/build-deps.sh:
#
#   Contents/MacOS/Scacelith              the game (<build dir>/scacelith), run path
#                                         @executable_path/../Frameworks
#   Contents/Frameworks/                  Mesa's libEGL.1.dylib, which the game loads, and what it
#                                         needs in turn (libgallium: Zink), the Vulkan loader that
#                                         Zink loads (libvulkan.1.dylib) and the driver the loader
#                                         loads (libvulkan_kosmickrisp.dylib); each found from
#                                         @rpath, with the run path @loader_path
#   Contents/Resources/vulkan/icd.d/      KosmicKrisp's manifest: the loader looks in the bundle's
#                                         Resources first, and the library path is relative to the
#                                         manifest
#   Contents/Resources/Scacelith.icns     the icon, made from res/icons/png/
#   Contents/Resources/LICENSE, licences/ the game's licence and the licence texts of what it
#                                         embeds or the bundle carries
#   Contents/Info.plist                   from res/macos/Info.plist.in
#
# then signs it ad hoc (arm64 code runs only signed; this signature names no developer, so
# Gatekeeper still asks the player to allow the app once) and checks it.
#
# Usage: tools/macos/make-app.sh <build dir> <dependency prefix> <output .app>
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=tools/macos/lib.sh
source "$ROOT/tools/macos/lib.sh"

[[ $# -eq 3 ]] || die "usage: $0 <build dir> <dependency prefix> <output .app>"
BUILD=$(cd "$1" && pwd) || die "no build folder $1"
DEPS=$(cd "$2" && pwd) || die "no dependency prefix $2 (tools/macos/build-deps.sh makes it)"
APP=$3
[[ "$APP" == *.app ]] || die "the output must be a .app folder, not $APP"
require_apple_silicon
read_version "$ROOT"

step "Scacelith.app $VERSION"
EXE=$BUILD/scacelith
[[ -f "$EXE" ]] || die "$EXE is missing (build the game first)"
require_arm64 "$EXE"
grep -aq "Scacelith $VERSION" "$EXE" || die "$EXE is not version $VERSION"
echo "$EXE: arm64, macOS $(macho_minos "$EXE")"

rm -rf "$APP"
CONTENTS=$APP/Contents
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Frameworks" "$CONTENTS/Resources/vulkan/icd.d" \
    "$CONTENTS/Resources/licences/fonts"
APP=$(cd "$APP" && pwd)
CONTENTS=$APP/Contents
FRAMEWORKS=$CONTENTS/Frameworks
RESOURCES=$CONTENTS/Resources
install -m 755 "$EXE" "$CONTENTS/MacOS/Scacelith"

# --- The libraries -------------------------------------------------------------------------------

# From the three the game reaches (libEGL by dlopen, libvulkan.1 by Zink's dlopen, KosmicKrisp by
# the loader's), and anything the executable itself links from @rpath, every @rpath dependency is
# copied in turn; a dependency that is neither macOS's nor in the prefix stops the script.
step "Frameworks"
find_lib() { # <install name> -> its file in the prefix
    local d
    for d in "$DEPS/mesa/lib" "$DEPS/vulkan/lib"; do
        [[ -e "$d/$1" ]] && { readlink -f "$d/$1"; return 0; }
    done
    return 1
}
queue=(libEGL.1.dylib libvulkan.1.dylib libvulkan_kosmickrisp.dylib)
while read -r dep; do
    [[ "$dep" == @rpath/* ]] && queue+=("${dep#@rpath/}")
done < <(macho_deps "$CONTENTS/MacOS/Scacelith")
# (An index rather than shifting the array: macOS's bash 3.2 treats an emptied array as unset
# under `set -u`.)
copied=" "
i=0
while ((i < ${#queue[@]})); do
    name=${queue[i]}
    i=$((i + 1))
    [[ "$copied" == *" $name "* ]] && continue
    src=$(find_lib "$name") || die "$name is not in $DEPS (rebuild it with tools/macos/build-deps.sh)"
    install -m 755 "$src" "$FRAMEWORKS/$name"
    copied+="$name "
    while read -r dep; do
        case "$dep" in
            /usr/lib/* | /System/Library/*) ;;
            @rpath/*) queue+=("${dep#@rpath/}") ;;
            *) die "$name needs $dep, which is neither part of macOS nor in the bundle" ;;
        esac
    done < <(macho_deps "$FRAMEWORKS/$name")
done
echo "bundled:$copied"

# KosmicKrisp's manifest, with the library given relative to the manifest's own folder
# (Contents/Resources/vulkan/icd.d -> Contents/Frameworks).
manifests=("$DEPS"/mesa/share/vulkan/icd.d/kosmickrisp*.json)
[[ ${#manifests[@]} -eq 1 && -f "${manifests[0]}" ]] || die "expected one KosmicKrisp manifest in $DEPS/mesa/share/vulkan/icd.d/"
/usr/bin/python3 -I - "${manifests[0]}" "$RESOURCES/vulkan/icd.d/kosmickrisp_icd.json" << 'EOF'
import json, sys
with open(sys.argv[1]) as f:
    manifest = json.load(f)
manifest["ICD"]["library_path"] = "../../../Frameworks/libvulkan_kosmickrisp.dylib"
with open(sys.argv[2], "w") as f:
    json.dump(manifest, f, indent=4)
    f.write("\n")
EOF
cat "$RESOURCES/vulkan/icd.d/kosmickrisp_icd.json"

# Run paths: the executable finds the libraries in ../Frameworks, each library its neighbours
# through @loader_path; any other run path (a folder of the build machine) is removed.
fix_rpaths() { # <Mach-O> <the run path it must have>
    local rp rps
    while read -r rp; do
        [[ "$rp" == "$2" ]] || install_name_tool -delete_rpath "$rp" "$1" 2> /dev/null
    done < <(macho_rpaths "$1")
    rps=$(macho_rpaths "$1")
    grep -qxF -- "$2" <<< "$rps" || install_name_tool -add_rpath "$2" "$1" 2> /dev/null
}
fix_rpaths "$CONTENTS/MacOS/Scacelith" @executable_path/../Frameworks
for f in "$FRAMEWORKS"/*.dylib; do
    [[ "$(macho_id "$f")" == "@rpath/$(basename "$f")" ]] || install_name_tool -id "@rpath/$(basename "$f")" "$f" 2> /dev/null
    fix_rpaths "$f" @loader_path
done

# --- Resources -----------------------------------------------------------------------------------

step "Icon, Info.plist and licences"
iconset=$(mktemp -d)/Scacelith.iconset
mkdir -p "$iconset"
for entry in 16:16x16 32:16x16@2x 32:32x32 64:32x32@2x 128:128x128 256:128x128@2x 256:256x256 \
             512:256x256@2x 512:512x512 1024:512x512@2x; do
    cp "$ROOT/res/icons/png/scacelith-${entry%%:*}.png" "$iconset/icon_${entry#*:}.png"
done
iconutil -c icns -o "$RESOURCES/Scacelith.icns" "$iconset" || die "iconutil could not make the icon"
rm -rf "$(dirname "$iconset")"

sed -e "s/@SCACELITH_VERSION@/$VERSION/g" -e "s/@SCACELITH_VERSION_CORE@/$VERSION_CORE/g" \
    "$ROOT/res/macos/Info.plist.in" > "$CONTENTS/Info.plist"
plutil -lint "$CONTENTS/Info.plist" > /dev/null || die "the Info.plist is not valid"
! grep -q '@SCACELITH_' "$CONTENTS/Info.plist" || die "a placeholder of Info.plist.in was left unreplaced"
printf 'APPL????' > "$CONTENTS/PkgInfo"

# The same licence texts as the Windows and Linux archives (README.md, "Licence"), and those of
# Mesa, the Vulkan loader, SPIRV-Tools (compiled into Mesa) and OpenSSL (linked into the game).
cp "$ROOT/LICENSE" "$RESOURCES/LICENSE"
cp "$ROOT"/assets/fonts/*.txt "$ROOT"/assets/fonts/hand/*.txt "$RESOURCES/licences/fonts/"
cp "$ROOT/third_party/stockfish/AUTHORS" "$RESOURCES/licences/Stockfish-AUTHORS.txt"
cp "$ROOT/third_party/qrcodegen/LICENSE" "$RESOURCES/licences/qrcodegen-MIT.txt"
cp "$ROOT/assets/licences/lichess-chess-openings-CC0.txt" "$RESOURCES/licences/"
cp -R "$DEPS/licences/." "$RESOURCES/licences/"
cp "$DEPS/PROVENANCE.txt" "$RESOURCES/licences/macOS-dependencies.txt"

# --- Checks and signature ------------------------------------------------------------------------

step "Checks"
bad=0
while IFS= read -r -d '' f; do
    [[ "$(file -b "$f")" == Mach-O* ]] || continue
    require_arm64 "$f"
    while read -r dep; do
        case "$dep" in
            /usr/lib/* | /System/Library/*) ;;
            @rpath/* | @loader_path/* | @executable_path/*)
                [[ -f "$FRAMEWORKS/$(basename "$dep")" ]] ||
                    { echo "${f#"$APP"/} needs $dep, which is not in Contents/Frameworks" >&2; bad=1; } ;;
            *) echo "${f#"$APP"/} needs $dep, outside macOS and the bundle" >&2; bad=1 ;;
        esac
    done < <(macho_deps "$f")
done < <(find "$APP" -type f -print0)
((bad == 0)) || die "the bundle's libraries do not resolve inside it (see above)"

# Inside out: the libraries, then the bundle (which seals Contents/ and signs the executable).
step "Signature (ad hoc)"
for f in "$FRAMEWORKS"/*.dylib; do
    codesign --force --sign - --timestamp=none "$f" || die "could not sign $f"
done
codesign --force --deep --sign - --timestamp=none "$APP" || die "could not sign $APP"
codesign --verify --deep --strict --verbose=2 "$APP" || die "the signature of $APP does not verify"
codesign --display --verbose=2 "$APP" 2>&1 | grep -E '^(Identifier|Format|Signature|CodeDirectory)' || true

step "What the bundle loads"
for f in "$CONTENTS/MacOS/Scacelith" "$FRAMEWORKS"/*.dylib; do
    otool -L "$f"
    echo "  run paths: $(macho_rpaths "$f" | tr '\n' ' ')  minimum macOS: $(macho_minos "$f")"
done
step_end
du -sh "$APP"
echo "$APP is ready."
