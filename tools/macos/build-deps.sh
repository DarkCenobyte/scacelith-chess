#!/usr/bin/env bash
# The third-party parts of the macOS (Apple Silicon) build, built from pinned sources into one
# folder, the dependency prefix, that the game's configure and tools/macos/make-app.sh read:
#
#   mesa/      Mesa 26.2.4 with the patches of tools/macos/mesa-patches/: OpenGL 4.6 through Zink
#              on KosmicKrisp (Vulkan on Metal 4, macOS 26 or later). libEGL.1.dylib (what the game
#              loads), libgallium-26.2.4.dylib (Zink), libvulkan_kosmickrisp.dylib and the driver's
#              manifest in share/vulkan/icd.d/.
#   vulkan/    the Khronos Vulkan loader, libvulkan.1.dylib, which Zink loads at run time from
#              @rpath (VK_LIBNAME in Mesa's zink_screen.c) and which finds KosmicKrisp through the
#              manifest in the app bundle.
#   openssl/   OpenSSL, static libraries and headers: the game's OPENSSL_ROOT_DIR.
#   licences/  their licence texts, which the app bundle carries.
#   PROVENANCE.txt  what was built from what: versions, commits, patch hashes, tools.
#
# Usage: tools/macos/build-deps.sh [<prefix>]           builds (default: build-macos-deps/prefix)
#        tools/macos/build-deps.sh --verify [<prefix>]  only checks a prefix (a restored cache)
#
# The sources and build trees go to $SCACELITH_DEPS_WORK (default: build-macos-deps/work), removed
# at the end unless SCACELITH_KEEP_WORK=1. Needs an Apple Silicon Mac, Xcode or its command line
# tools with the macOS 26 SDK, Homebrew (the build tools below are installed when missing) and
# Python 3.10 or later. Most of the time goes to Mesa and to the SPIR-V translator.
#
# What is shipped (Mesa's libraries and the loader) must load from the app bundle on any Mac, so
# they are installed from neutral prefixes (/opt/scacelith-*, which exist nowhere: no path of the
# build machine gets compiled in), their install names are rewritten to @rpath, their only run
# path is @loader_path, and the checks at the end (also run by --verify) fail the build when a
# library is not arm64 code for macOS 26, needs a library other than macOS's own and the bundled
# ones, or still holds a path of the build machine (Homebrew, the home or work folders).
#
# The pins below are updated by hand (tools/macos/README.md says how); a change of this file or
# of the patches gives the CI's cache of the prefix a new key, so the next run rebuilds it.
set -euo pipefail

# --- Pins ----------------------------------------------------------------------------------------

# Mesa: the latest stable release, from its Git tag (the commit is checked), else from the release
# tarball (freedesktop.org's GitLab sometimes turns CI machines away), whose SHA-256 is the one of
# Homebrew's mesa formula for the same release.
MESA_VERSION=26.2.4
MESA_GIT=https://gitlab.freedesktop.org/mesa/mesa.git
MESA_TAG=mesa-26.2.4
MESA_COMMIT=96cb43121031992b85767f9c1be8f3f48e22b1d2
MESA_TARBALL=https://archive.mesa3d.org/mesa-26.2.4.tar.xz
MESA_TARBALL_SHA256=bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9

# The Khronos Vulkan loader and the headers it is released with.
VULKAN_HEADERS_TAG=v1.4.365
VULKAN_HEADERS_COMMIT=c46850864f4661461b0f6cb9922c058ffea4915e
VULKAN_LOADER_TAG=v1.4.365
VULKAN_LOADER_COMMIT=f866657ff687a36d767cd7783bab800e31ebafaa

# OpenSSL, linked statically into the game.
OPENSSL_VERSION=3.6.5
OPENSSL_TARBALL=https://github.com/openssl/openssl/releases/download/openssl-3.6.5/openssl-3.6.5.tar.gz
OPENSSL_SHA256=a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98

# Build-time only (nothing of them is shipped, except what SPIRV-Tools compiles into Mesa).
# KosmicKrisp's kernels are OpenCL C, compiled while Mesa builds by mesa_clc with Clang, LLVM and
# the SPIR-V translator, linked statically. LLVM 19: the major known to build and run this stack
# (RecoilEngine-AppleSilicon ships Mesa 26.2 with KosmicKrisp built so, with LLVM 19 and the
# translator v19.1.7, and its patches are those of tools/macos/mesa-patches/). Homebrew's current
# llvm is built with link-time optimisation and its static libraries failed to link mesa_clc there;
# the versioned llvm@19 is a plain build. The translator must match LLVM's major and minor version.
LLVM_FORMULA=llvm@19
LLVM_MAJOR=19
SPIRV_LLVM_TRANSLATOR_TAG=v19.1.7
SPIRV_LLVM_TRANSLATOR_COMMIT=3fac86cf760a4a2510f3c1e76001ea799812a2f6
# SPIRV-Tools, built as static libraries: Mesa's SPIR-V front end links it, and Homebrew's is a
# shared library, which the bundle would then need too (KosmicKrisp's documentation also asks for
# the static one).
SPIRV_HEADERS_TAG=vulkan-sdk-1.4.363.0
SPIRV_HEADERS_COMMIT=496543121ce6419f23d6fa5d7194ba66c36212d2
SPIRV_TOOLS_TAG=vulkan-sdk-1.4.363.0
SPIRV_TOOLS_COMMIT=ef96ed763b43b59b33b31b362f09a02b729fa1c9

# Homebrew formulae for the build: CMake, Ninja, pkg-config, a bison newer than macOS's 2.3,
# LLVM, and MoltenVK, whose headers Zink's build requires on macOS (MoltenVK itself is neither
# used nor shipped: KosmicKrisp is the Vulkan driver).
BREW_FORMULAE=(cmake ninja pkgconf bison "$LLVM_FORMULA" molten-vk)

MACOS_MIN=26.0

# --- Setup ----------------------------------------------------------------------------------------

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=tools/macos/lib.sh
source "$ROOT/tools/macos/lib.sh"

VERIFY_ONLY=false
if [[ "${1:-}" == --verify ]]; then
    VERIFY_ONLY=true
    shift
fi
PREFIX=${1:-$ROOT/build-macos-deps/prefix}
WORK=${SCACELITH_DEPS_WORK:-$ROOT/build-macos-deps/work}

MESA_NEUTRAL=/opt/scacelith-mesa
VULKAN_NEUTRAL=/opt/scacelith-vulkan
OPENSSL_NEUTRAL=/opt/scacelith-openssl

# The libraries the app bundle ships, by install name: what may appear as @rpath/<name>.
BUNDLED_NAMES=(libEGL.1.dylib "libgallium-$MESA_VERSION.dylib" libvulkan_kosmickrisp.dylib libvulkan.1.dylib)

retry() { # <attempts> <command...>: network operations, retried after a pause
    local n=$1 i
    shift
    for ((i = 1; i <= n; i++)); do
        "$@" && return 0
        ((i < n)) && { warn "attempt $i of $n failed: $*"; sleep $((i * 10)); }
    done
    return 1
}

# A shallow clone of a tag, checked against its pinned commit (a tag that moved stops the build).
shallow_clone() { # <url> <tag> <dir>
    rm -rf "$3"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$2" "$1" "$3"
}
clone_pinned() { # <url> <tag> <commit> <dir>
    local head
    retry 3 shallow_clone "$1" "$2" "$4" || return 1
    head=$(git -C "$4" rev-parse HEAD)
    [[ "$head" == "$3" ]] || die "$1: tag $2 is at $head, not at the pinned $3 (was the tag moved?)"
    echo "$(basename "$4"): $2 = $3"
}

download_checked() { # <url> <sha256> <file>
    retry 3 curl -fsSL -o "$3" "$1" || die "could not download $1"
    echo "$2  $3" | shasum -a 256 -c - > /dev/null ||
        die "$1 does not have the pinned SHA-256 $2 (got $(shasum -a 256 "$3" | cut -d' ' -f1))"
    echo "$(basename "$3"): SHA-256 $2"
}

# --- Checks of a prefix (the end of a build, and --verify) -------------------------------------------

verify_prefix() {
    local p=$1 f name dep rp hits bad=0 n=0
    step "Checking the dependency prefix $p"
    [[ -d "$p" ]] || die "$p does not exist"
    for f in mesa/lib/libEGL.1.dylib "mesa/lib/libgallium-$MESA_VERSION.dylib" \
             mesa/lib/libvulkan_kosmickrisp.dylib vulkan/lib/libvulkan.1.dylib \
             openssl/lib/libssl.a openssl/lib/libcrypto.a openssl/include/openssl/ssl.h \
             licences/Mesa/license.rst licences/Vulkan-Loader-LICENSE.txt \
             licences/OpenSSL-LICENSE.txt PROVENANCE.txt; do
        [[ -e "$p/$f" ]] || die "$p/$f is missing"
    done
    compgen -G "$p/mesa/share/vulkan/icd.d/kosmickrisp*.json" > /dev/null ||
        die "KosmicKrisp's manifest is missing from $p/mesa/share/vulkan/icd.d/"
    grep -q "define OPENSSL_VERSION_STR \"$OPENSSL_VERSION\"" "$p/openssl/include/openssl/opensslv.h" ||
        die "$p/openssl holds another OpenSSL than $OPENSSL_VERSION"

    # The shipped libraries: arm64 for macOS 26, @rpath install names, dependencies on macOS's own
    # libraries or on one another only, run paths relative to the library.
    for name in "${BUNDLED_NAMES[@]}"; do
        if [[ -e "$p/mesa/lib/$name" ]]; then f="$p/mesa/lib/$name"; else f="$p/vulkan/lib/$name"; fi
        f=$(readlink -f "$f") # the file a link such as libvulkan.1.dylib names
        require_arm64 "$f"
        [[ "$(macho_minos "$f")" == "$MACOS_MIN" ]] ||
            die "$name is built for macOS $(macho_minos "$f"), not $MACOS_MIN"
        [[ "$(macho_id "$f")" == "@rpath/$name" ]] ||
            die "$name has the install name $(macho_id "$f"), not @rpath/$name"
        while read -r dep; do
            case "$dep" in
                /usr/lib/* | /System/Library/*) ;;
                @rpath/*)
                    [[ " ${BUNDLED_NAMES[*]} " == *" ${dep#@rpath/} "* ]] ||
                        { echo "$name needs $dep, which the bundle does not ship" >&2; bad=1; } ;;
                *) echo "$name needs $dep, outside macOS and the bundle" >&2; bad=1 ;;
            esac
        done < <(macho_deps "$f")
        while read -r rp; do
            [[ "$rp" == @loader_path* || "$rp" == @executable_path* ]] ||
                { echo "$name has the run path $rp" >&2; bad=1; }
        done < <(macho_rpaths "$f")
        codesign --verify "$f" 2> /dev/null || { echo "$name is not signed (ad hoc) after its changes" >&2; bad=1; }
        n=$((n + 1))
    done
    ((bad == 0)) || die "the shipped libraries of $p fail the checks above"

    # No path of the build machine in what is shipped or linked into the game (raw bytes: `strings`
    # does not read every section).
    for f in "$p"/mesa/lib/*.dylib "$p"/vulkan/lib/*.dylib "$p"/openssl/lib/*.a; do
        [[ -L "$f" ]] && continue
        hits=$(LC_ALL=C grep -a -o -E '(/Users/|/opt/homebrew|/usr/local/(Cellar|opt)/|/private/var/folders/)[^"[:cntrl:]]{0,80}' \
            "$f" | sort -u | head -n 5 || true)
        if [[ -n "$hits" ]]; then
            echo "$(basename "$f") holds paths of the build machine:" >&2
            printf '    %s\n' "${hits//$'\n'/$'\n'    }" >&2
            bad=1
        fi
    done
    ((bad == 0)) || die "$p holds paths of the build machine (see above)"
    for f in "$p/openssl/lib/libssl.a" "$p/openssl/lib/libcrypto.a"; do
        [[ "$(lipo -archs "$f")" == arm64 ]] || die "$f is not arm64 alone ($(lipo -archs "$f"))"
    done
    echo "$n shipped libraries checked: arm64, macOS $MACOS_MIN, @rpath, no foreign dependency or path"
    step_end
}

if [[ "$VERIFY_ONLY" == true ]]; then
    require_apple_silicon
    verify_prefix "$(cd "$PREFIX" 2> /dev/null && pwd || echo "$PREFIX")"
    cat "$PREFIX/PROVENANCE.txt"
    exit 0
fi

# --- Preflight -----------------------------------------------------------------------------------

step "Checking the build machine"
require_apple_silicon
command -v xcrun > /dev/null || die "the Xcode command line tools are missing (xcode-select --install)"
SDK_VERSION=$(xcrun --sdk macosx --show-sdk-version) || die "no macOS SDK (xcode-select --install)"
((${SDK_VERSION%%.*} >= 26)) || die "the macOS SDK is $SDK_VERSION: KosmicKrisp needs Metal 4, from the macOS 26 SDK"
command -v brew > /dev/null || die "Homebrew is needed for the build tools (https://brew.sh)"
for tool in git curl shasum; do
    command -v "$tool" > /dev/null || die "$tool is needed"
done
export MACOSX_DEPLOYMENT_TARGET=$MACOS_MIN
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
echo "macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), SDK $SDK_VERSION, $(xcodebuild -version 2> /dev/null | head -n 1 || echo 'command line tools'), $JOBS jobs"
/usr/bin/clang --version || die "no /usr/bin/clang: install Xcode or its command line tools"

step "Homebrew build tools: ${BREW_FORMULAE[*]}"
missing=()
for formula in "${BREW_FORMULAE[@]}"; do
    brew list --formula --versions "$formula" > /dev/null 2>&1 || missing+=("$formula")
done
if ((${#missing[@]})); then
    brew install --formula --quiet "${missing[@]}" || die "brew install ${missing[*]} failed"
fi
for formula in "${BREW_FORMULAE[@]}"; do brew list --formula --versions "$formula"; done
BREW_PREFIX=$(brew --prefix)
LLVM_PREFIX=$(brew --prefix "$LLVM_FORMULA")
ZSTD_PREFIX=$(brew --prefix zstd) # a dependency of llvm@19: its static libraries name -lzstd
MOLTENVK_PREFIX=$(brew --prefix molten-vk)
llvm_version=$("$LLVM_PREFIX/bin/llvm-config" --version) || die "$LLVM_FORMULA has no llvm-config"
[[ "$llvm_version" == "$LLVM_MAJOR".* ]] || die "$LLVM_FORMULA is LLVM $llvm_version, not $LLVM_MAJOR"
[[ -f "$LLVM_PREFIX/lib/libclangBasic.a" && -f "$LLVM_PREFIX/lib/libLLVMCore.a" ]] ||
    die "$LLVM_FORMULA has no static Clang and LLVM libraries, which mesa_clc links"
echo "LLVM $llvm_version at $LLVM_PREFIX"

rm -rf "$WORK" "$PREFIX"
mkdir -p "$WORK/src" "$WORK/build" "$WORK/stage" "$PREFIX/licences"
TOOLS=$WORK/tools # build-time installs (SPIRV-Tools, the translator, the Vulkan headers)

step "Python packages of Mesa's build"
make_venv "$WORK/venv" "$ROOT/tools/macos/requirements-mesa.txt"
"$WORK/venv/bin/python" --version
"$WORK/venv/bin/meson" --version

# Every CMake project with the same compilers and target as Mesa.
cmake_build() { # <source> <build> <install prefix> [cmake options...]
    local src=$1 bld=$2 inst=$3
    shift 3
    cmake -S "$src" -B "$bld" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$inst" \
        -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN" "$@" > "$bld.configure.log" 2>&1 ||
        { tail -n 60 "$bld.configure.log"; die "configuring $(basename "$src") failed"; }
    cmake --build "$bld" --parallel "$JOBS" || die "building $(basename "$src") failed"
}

# --- SPIRV-Tools (static) ------------------------------------------------------------------------

step "SPIRV-Tools $SPIRV_TOOLS_TAG (static, build time)"
clone_pinned https://github.com/KhronosGroup/SPIRV-Headers.git "$SPIRV_HEADERS_TAG" "$SPIRV_HEADERS_COMMIT" \
    "$WORK/src/SPIRV-Headers" || die "could not clone SPIRV-Headers"
clone_pinned https://github.com/KhronosGroup/SPIRV-Tools.git "$SPIRV_TOOLS_TAG" "$SPIRV_TOOLS_COMMIT" \
    "$WORK/src/SPIRV-Tools" || die "could not clone SPIRV-Tools"
cmake_build "$WORK/src/SPIRV-Tools" "$WORK/build/SPIRV-Tools" "$TOOLS" \
    -DBUILD_SHARED_LIBS=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DSPIRV_SKIP_EXECUTABLES=ON \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DSPIRV-Headers_SOURCE_DIR="$WORK/src/SPIRV-Headers" \
    -DPython3_EXECUTABLE="$WORK/venv/bin/python"
cmake --install "$WORK/build/SPIRV-Tools" > /dev/null
[[ -f "$TOOLS/lib/libSPIRV-Tools.a" && -f "$TOOLS/lib/pkgconfig/SPIRV-Tools.pc" ]] ||
    die "SPIRV-Tools did not install its static library and pkg-config file"

# --- SPIRV-LLVM-Translator -----------------------------------------------------------------------

step "SPIRV-LLVM-Translator $SPIRV_LLVM_TRANSLATOR_TAG with LLVM $llvm_version (build time)"
clone_pinned https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git "$SPIRV_LLVM_TRANSLATOR_TAG" \
    "$SPIRV_LLVM_TRANSLATOR_COMMIT" "$WORK/src/SPIRV-LLVM-Translator" || die "could not clone SPIRV-LLVM-Translator"
# Its configure fetches SPIRV-Headers at the commit named in its spirv-headers-tag.conf. -lzstd
# comes from LLVM's static libraries; -lto_library names LLVM's own LTO plugin to the linker in
# case one of them holds bitcode.
llvm_link_flags="-L$ZSTD_PREFIX/lib -Wl,-lto_library,$LLVM_PREFIX/lib/libLTO.dylib"
cmake_build "$WORK/src/SPIRV-LLVM-Translator" "$WORK/build/SPIRV-LLVM-Translator" "$TOOLS" \
    -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" -DLLVM_SPIRV_INCLUDE_TESTS=OFF -DCCACHE_ALLOWED=OFF \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_EXE_LINKER_FLAGS="$llvm_link_flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$llvm_link_flags"
cmake --install "$WORK/build/SPIRV-LLVM-Translator" > /dev/null
[[ -f "$TOOLS/lib/pkgconfig/LLVMSPIRVLib.pc" ]] || die "the translator did not install LLVMSPIRVLib.pc"

# --- Mesa ----------------------------------------------------------------------------------------

step "Mesa $MESA_VERSION (Zink and KosmicKrisp)"
MESA_SRC=$WORK/src/mesa
mesa_origin="git $MESA_GIT $MESA_TAG ($MESA_COMMIT)"
if ! clone_pinned "$MESA_GIT" "$MESA_TAG" "$MESA_COMMIT" "$MESA_SRC"; then
    warn "could not clone $MESA_GIT: taking the release tarball instead"
    download_checked "$MESA_TARBALL" "$MESA_TARBALL_SHA256" "$WORK/mesa.tar.xz"
    rm -rf "$MESA_SRC" && mkdir -p "$MESA_SRC"
    tar -xJf "$WORK/mesa.tar.xz" -C "$MESA_SRC" --strip-components 1
    # A repository of its own, so that git apply below patches this tree and not the enclosing
    # one (the work folder may lie inside the game's checkout).
    git -C "$MESA_SRC" init --quiet
    mesa_origin="$MESA_TARBALL (SHA-256 $MESA_TARBALL_SHA256)"
fi
[[ "$(cat "$MESA_SRC/VERSION")" == "$MESA_VERSION" ]] || die "the Mesa sources are version $(cat "$MESA_SRC/VERSION")"
patches=("$ROOT"/tools/macos/mesa-patches/*.patch)
for patch in "${patches[@]}"; do
    echo "applying $(basename "$patch")"
    git -C "$MESA_SRC" apply --whitespace=nowarn "$patch" ||
        die "$(basename "$patch") does not apply to Mesa $MESA_VERSION (merged upstream? see tools/macos/mesa-patches/README.md)"
done

# Apple's Clang builds Mesa; llvm-config is the one of LLVM 19. The source paths compiled in
# (__FILE__) are made relative to the work folder.
cat > "$WORK/mesa-native.ini" << EOF
[binaries]
c = '/usr/bin/clang'
cpp = '/usr/bin/clang++'
objc = '/usr/bin/clang'
objcpp = '/usr/bin/clang++'
llvm-config = '$LLVM_PREFIX/bin/llvm-config'
pkg-config = '$BREW_PREFIX/bin/pkg-config'

[built-in options]
c_args = ['-ffile-prefix-map=$WORK=.']
cpp_args = ['-ffile-prefix-map=$WORK=.']
objc_args = ['-ffile-prefix-map=$WORK=.']
objcpp_args = ['-ffile-prefix-map=$WORK=.']
c_link_args = ['-L$ZSTD_PREFIX/lib', '-Wl,-lto_library,$LLVM_PREFIX/lib/libLTO.dylib']
cpp_link_args = ['-L$ZSTD_PREFIX/lib', '-Wl,-lto_library,$LLVM_PREFIX/lib/libLTO.dylib']
objc_link_args = ['-L$ZSTD_PREFIX/lib', '-Wl,-lto_library,$LLVM_PREFIX/lib/libLTO.dylib']
objcpp_link_args = ['-L$ZSTD_PREFIX/lib', '-Wl,-lto_library,$LLVM_PREFIX/lib/libLTO.dylib']
EOF

# The options, beyond the drivers:
# - EGL without a window system (surfaceless; the game shows its frames itself), desktop OpenGL
#   only (no GLES, no GLX, no GLVND).
# - LLVM for the build-time CLC compiler only, linked statically: no llvmpipe, and no LLVM in the
#   draw module (draw-use-llvm=false), so the shipped libraries do not carry LLVM.
# - Zink loads the Vulkan loader as @rpath/libvulkan.1.dylib, and finds it next to itself
#   (vulkan-loader-rpath=@loader_path: both live in Contents/Frameworks).
# - No dependency the bundle would have to carry: no zstd, no expat (the driver configuration is
#   built in: xmlconfig=disabled), no libunwind or valgrind.
mesa_options=(
    --prefix="$MESA_NEUTRAL" --libdir=lib --buildtype=release -Db_ndebug=true -Dstrip=true
    --pkg-config-path="$TOOLS/lib/pkgconfig"
    -Dplatforms=macos -Degl=enabled -Degl-native-platform=surfaceless -Dglx=disabled
    -Dopengl=true -Dgles1=disabled -Dgles2=disabled -Dglvnd=disabled -Dgbm=disabled
    -Dgallium-drivers=zink -Dvulkan-drivers=kosmickrisp
    -Dllvm=enabled -Dshared-llvm=disabled -Ddraw-use-llvm=false
    -Dmoltenvk-dir="$MOLTENVK_PREFIX" -Dvulkan-loader-rpath=@loader_path
    -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dlibunwind=disabled -Dvalgrind=disabled
    -Dlmsensors=disabled -Dbuild-tests=false
)
echo "meson setup ${mesa_options[*]}"
(
    bison_bin=$(brew --prefix bison)/bin
    export PATH="$WORK/venv/bin:$bison_bin:$BREW_PREFIX/bin:$PATH"
    meson setup "$WORK/build/mesa" "$MESA_SRC" --native-file "$WORK/mesa-native.ini" "${mesa_options[@]}" ||
        die "configuring Mesa failed (the log: $WORK/build/mesa/meson-logs/meson-log.txt)"
    meson compile -C "$WORK/build/mesa" -j "$JOBS" || die "building Mesa failed"
    meson install -C "$WORK/build/mesa" --no-rebuild --destdir "$WORK/stage/mesa" > /dev/null ||
        die "installing Mesa failed"
)
mkdir -p "$PREFIX/mesa"
cp -a "$WORK/stage/mesa$MESA_NEUTRAL/." "$PREFIX/mesa/"
mkdir -p "$PREFIX/licences/Mesa"
cp "$MESA_SRC/docs/license.rst" "$PREFIX/licences/Mesa/license.rst"
cp -R "$MESA_SRC/licenses/." "$PREFIX/licences/Mesa/"
ls -l "$PREFIX/mesa/lib/" "$PREFIX/mesa/share/vulkan/icd.d/" || true

# --- Vulkan loader -------------------------------------------------------------------------------

step "Vulkan loader $VULKAN_LOADER_TAG"
clone_pinned https://github.com/KhronosGroup/Vulkan-Headers.git "$VULKAN_HEADERS_TAG" "$VULKAN_HEADERS_COMMIT" \
    "$WORK/src/Vulkan-Headers" || die "could not clone Vulkan-Headers"
clone_pinned https://github.com/KhronosGroup/Vulkan-Loader.git "$VULKAN_LOADER_TAG" "$VULKAN_LOADER_COMMIT" \
    "$WORK/src/Vulkan-Loader" || die "could not clone Vulkan-Loader"
cmake_build "$WORK/src/Vulkan-Headers" "$WORK/build/Vulkan-Headers" "$TOOLS"
cmake --install "$WORK/build/Vulkan-Headers" > /dev/null
# SYSCONFDIR=/etc: the system-wide manifests are looked for in /etc, not under the build's prefix.
cmake_build "$WORK/src/Vulkan-Loader" "$WORK/build/Vulkan-Loader" "$VULKAN_NEUTRAL" \
    -DCMAKE_PREFIX_PATH="$TOOLS" -DBUILD_TESTS=OFF -DSYSCONFDIR=/etc
DESTDIR="$WORK/stage/vulkan" cmake --install "$WORK/build/Vulkan-Loader" > /dev/null
mkdir -p "$PREFIX/vulkan/lib"
# The library and its links (libvulkan.1.dylib -> libvulkan.1.4.365.dylib); not the framework
# copy, the CMake package or the pkg-config file.
cp -a "$WORK/stage/vulkan$VULKAN_NEUTRAL"/lib/libvulkan*.dylib "$PREFIX/vulkan/lib/"
cp "$WORK/src/Vulkan-Loader/LICENSE.txt" "$PREFIX/licences/Vulkan-Loader-LICENSE.txt"
cp "$WORK/src/SPIRV-Tools/LICENSE" "$PREFIX/licences/SPIRV-Tools-LICENSE.txt"
cp "$WORK/src/SPIRV-Headers/LICENSE" "$PREFIX/licences/SPIRV-Headers-LICENSE.txt"
ls -l "$PREFIX/vulkan/lib/"

# --- OpenSSL -------------------------------------------------------------------------------------

# Static, for the game alone. OPENSSLDIR=/etc/ssl: OpenSSL's default certificate store is then
# /etc/ssl/cert.pem, the bundle of trusted roots macOS keeps there.
step "OpenSSL $OPENSSL_VERSION (static)"
download_checked "$OPENSSL_TARBALL" "$OPENSSL_SHA256" "$WORK/openssl.tar.gz"
tar -xzf "$WORK/openssl.tar.gz" -C "$WORK/src"
(
    cd "$WORK/src/openssl-$OPENSSL_VERSION"
    ./Configure darwin64-arm64-cc no-shared no-module no-tests --prefix="$OPENSSL_NEUTRAL" --libdir=lib \
        --openssldir=/etc/ssl "-mmacosx-version-min=$MACOS_MIN" > "$WORK/build/openssl.configure.log" 2>&1 ||
        { tail -n 40 "$WORK/build/openssl.configure.log"; die "configuring OpenSSL failed"; }
    make -j "$JOBS" build_libs > "$WORK/build/openssl.build.log" 2>&1 ||
        { tail -n 60 "$WORK/build/openssl.build.log"; die "building OpenSSL failed"; }
    make install_dev DESTDIR="$WORK/stage/openssl" > /dev/null || die "installing OpenSSL failed"
)
mkdir -p "$PREFIX/openssl"
cp -a "$WORK/stage/openssl$OPENSSL_NEUTRAL/." "$PREFIX/openssl/"
# The pkg-config files name the neutral prefix: point them at this one (they are not shipped).
sed -i '' "s|^prefix=.*|prefix=$PREFIX/openssl|" "$PREFIX"/openssl/lib/pkgconfig/*.pc
cp "$WORK/src/openssl-$OPENSSL_VERSION/LICENSE.txt" "$PREFIX/licences/OpenSSL-LICENSE.txt"
ls -l "$PREFIX/openssl/lib/"

# --- Relocation ----------------------------------------------------------------------------------

# Meson's install wrote the neutral prefix into the install names (LC_ID_DYLIB) and into the
# libraries' references to one another: every one becomes @rpath/<name>, the only run path is
# @loader_path (the bundle's Frameworks folder holds them all; the executable adds its own), and
# each library is signed again (ad hoc: arm64 code runs only signed, and install_name_tool voids
# the linker's signature).
step "Install names, run paths and signatures"
for f in "$PREFIX"/mesa/lib/*.dylib "$PREFIX"/vulkan/lib/*.dylib; do
    [[ -L "$f" ]] && continue
    chmod u+w "$f"
    id=$(macho_id "$f")
    [[ -n "$id" ]] || die "$f has no install name"
    install_name_tool -id "@rpath/$(basename "$id")" "$f" 2> /dev/null
    while read -r dep; do
        case "$dep" in
            "$MESA_NEUTRAL"/* | "$VULKAN_NEUTRAL"/*) install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$f" 2> /dev/null ;;
        esac
    done < <(macho_deps "$f")
    while read -r rp; do
        [[ "$rp" == @loader_path* || "$rp" == @executable_path* ]] || install_name_tool -delete_rpath "$rp" "$f" 2> /dev/null
    done < <(macho_rpaths "$f")
    rps=$(macho_rpaths "$f")
    grep -qx '@loader_path' <<< "$rps" || install_name_tool -add_rpath @loader_path "$f" 2> /dev/null
    codesign --force --sign - "$f" 2> /dev/null || die "could not sign $f"
    echo "$(basename "$f"): $(macho_id "$f"), needs: $(macho_deps "$f" | tr '\n' ' ')"
done

# --- Provenance ----------------------------------------------------------------------------------

{
    echo "Third-party parts of Scacelith's macOS build (tools/macos/build-deps.sh), built $(date -u +%Y-%m-%dT%H:%MZ)"
    echo
    echo "Mesa $MESA_VERSION: $mesa_origin"
    for patch in "${patches[@]}"; do
        echo "  + $(basename "$patch") (SHA-256 $(shasum -a 256 "$patch" | cut -d' ' -f1))"
    done
    echo "  meson ${mesa_options[*]//$PREFIX/<prefix>}" | sed "s|$WORK|<work>|g; s|$BREW_PREFIX|<homebrew>|g"
    echo "Vulkan loader: Vulkan-Loader $VULKAN_LOADER_TAG ($VULKAN_LOADER_COMMIT), Vulkan-Headers $VULKAN_HEADERS_TAG ($VULKAN_HEADERS_COMMIT)"
    echo "OpenSSL $OPENSSL_VERSION: $OPENSSL_TARBALL (SHA-256 $OPENSSL_SHA256), static"
    echo
    echo "Build time only: LLVM $llvm_version (Homebrew $LLVM_FORMULA), SPIRV-LLVM-Translator $SPIRV_LLVM_TRANSLATOR_TAG ($SPIRV_LLVM_TRANSLATOR_COMMIT),"
    echo "  SPIRV-Tools $SPIRV_TOOLS_TAG ($SPIRV_TOOLS_COMMIT, static, linked into Mesa), SPIRV-Headers $SPIRV_HEADERS_TAG ($SPIRV_HEADERS_COMMIT),"
    echo "  MoltenVK headers (Homebrew molten-vk $(brew list --formula --versions molten-vk | cut -d' ' -f2))"
    echo "Tools: macOS $(sw_vers -productVersion), SDK $SDK_VERSION, $(/usr/bin/clang --version | head -n 1), meson $("$WORK/venv/bin/meson" --version)"
} > "$PREFIX/PROVENANCE.txt"

verify_prefix "$PREFIX"
cat "$PREFIX/PROVENANCE.txt"
if [[ "${SCACELITH_KEEP_WORK:-0}" != 1 ]]; then
    rm -rf "$WORK"
fi
echo "The macOS dependencies are in $PREFIX (OPENSSL_ROOT_DIR=$PREFIX/openssl)."
