# Helpers shared by the macOS scripts of this folder (sourced, not run). They fail loudly: every
# error names what is missing and stops the script, so that a CI log says at once what to fix.

# An error, as a GitHub Actions annotation when the script runs in a workflow, and the end.
die() {
    if [[ "${GITHUB_ACTIONS:-}" == true ]]; then
        echo "::error::$(basename "$0"): $*" >&2
    else
        echo "$(basename "$0"): error: $*" >&2
    fi
    exit 1
}

warn() {
    if [[ "${GITHUB_ACTIONS:-}" == true ]]; then
        echo "::warning::$(basename "$0"): $*" >&2
    else
        echo "$(basename "$0"): warning: $*" >&2
    fi
}

# A titled step, folded in the workflow log.
step_open=false
step() {
    if [[ "${GITHUB_ACTIONS:-}" == true ]]; then
        [[ "$step_open" == true ]] && echo "::endgroup::"
        echo "::group::$*"
        step_open=true
    else
        echo "=== $*"
    fi
}
step_end() {
    if [[ "${GITHUB_ACTIONS:-}" == true && "$step_open" == true ]]; then
        echo "::endgroup::"
        step_open=false
    fi
}

# Apple Silicon only: KosmicKrisp is a Metal 4 driver for Apple GPUs, and the game is built for
# arm64 alone. `uname -m` also tells a shell running under Rosetta (x86_64), which would build
# Intel code.
require_apple_silicon() {
    [[ "$(uname -s)" == Darwin ]] || die "this script runs on macOS only (this is $(uname -s))"
    [[ "$(uname -m)" == arm64 ]] ||
        die "Apple Silicon only: this shell runs as $(uname -m) (an Intel Mac, or a shell under Rosetta)"
}

# A Python 3.10 or later (Meson and dmgbuild need it; macOS's own python3 is 3.9): python3 when
# it is recent enough, else the newest python3.N on the PATH.
find_python() {
    local p
    for p in python3 python3.15 python3.14 python3.13 python3.12 python3.11 python3.10; do
        command -v "$p" > /dev/null 2>&1 || continue
        if "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2> /dev/null; then
            command -v "$p"
            return 0
        fi
    done
    return 1
}

# A virtual environment with the packages of a requirements file, every file checked against its
# pinned SHA-256 and only wheels accepted (no package code runs at install time).
make_venv() { # <venv dir> <requirements file>
    local python
    python=$(find_python) || die "no Python 3.10 or later on the PATH (brew install python)"
    rm -rf "$1"
    "$python" -m venv "$1" || die "could not create a virtual environment with $python"
    PIP_DISABLE_PIP_VERSION_CHECK=1 "$1/bin/python" -m pip install --quiet --require-hashes \
        --only-binary :all: -r "$2" || die "pip could not install the pinned packages of $2"
}

# The load commands of a Mach-O file, from `otool -l` (paths hold no spaces in what is checked here):
# the libraries it loads, its own install name and its run paths.
macho_deps() {
    otool -l "$1" | awk '$1 == "cmd" { c = $2 }
        $1 == "name" && c ~ /^LC_(LOAD|LOAD_WEAK|REEXPORT|LAZY_LOAD|LOAD_UPWARD)_DYLIB$/ { print $2 }'
}
macho_id() {
    otool -l "$1" | awk '$1 == "cmd" { c = $2 } $1 == "name" && c == "LC_ID_DYLIB" { print $2 }'
}
macho_rpaths() {
    otool -l "$1" | awk '$1 == "cmd" { c = $2 } $1 == "path" && c == "LC_RPATH" { print $2 }'
}
# The minimum macOS version a Mach-O file was built for (LC_BUILD_VERSION).
macho_minos() {
    otool -l "$1" | awk '$1 == "cmd" { c = $2 } $1 == "minos" && c == "LC_BUILD_VERSION" && !n++ { print $2 }'
}

# Fails unless the file holds arm64 code only.
require_arm64() {
    local archs
    archs=$(lipo -archs "$1" 2> /dev/null) || die "$1 is not a Mach-O file"
    [[ "$archs" == arm64 ]] || die "$1 is built for '$archs', not arm64 alone"
}

# The game's version (cmake/version.cmake), and its MAJOR.MINOR.PATCH part (CFBundleShortVersionString
# takes numbers only), in VERSION and VERSION_CORE.
# shellcheck disable=SC2034 # used by the scripts that source this file
read_version() { # <repository root>
    command -v cmake > /dev/null || die "cmake is needed to read cmake/version.cmake"
    VERSION=$(cmake -P "$1/cmake/version.cmake") || die "cmake -P cmake/version.cmake failed"
    [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] ||
        die "cmake/version.cmake gives '$VERSION', not a SemVer version"
    VERSION_CORE=${VERSION%%-*}
}
