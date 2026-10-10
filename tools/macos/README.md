# The macOS build (Apple Silicon, experimental)

The scripts of this folder make the macOS release, `Scacelith-<version>-macos-arm64-experimental.dmg`:
Scacelith.app for Apple Silicon Macs (M1 or later) with macOS 26 or later. macOS stops at OpenGL
4.1, and the game needs 4.6: the app carries its own OpenGL, Mesa's Zink (OpenGL on Vulkan) on
KosmicKrisp (Mesa's Vulkan driver on Metal 4, which is why macOS 26 is needed).

```sh
tools/macos/build-deps.sh       # once: Mesa, the Vulkan loader, OpenSSL -> build-macos-deps/prefix
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DOPENSSL_ROOT_DIR="$PWD/build-macos-deps/prefix/openssl"
ninja -C build && ./build/scacelith_tests
tools/macos/make-app.sh build build-macos-deps/prefix build-macos/Scacelith.app
tools/macos/make-dmg.sh build-macos/Scacelith.app   # -> build-macos/Scacelith-<version>-macos-arm64-experimental.dmg
```

They need an Apple Silicon Mac (a shell running under Rosetta is refused), Xcode or its command
line tools with the macOS 26 SDK, [Homebrew](https://brew.sh) for the build tools (`build-deps.sh`
installs the formulae it lacks: cmake, ninja, pkgconf, bison, llvm@19, molten-vk) and Python 3.10
or later. Each script stops at the first problem with a message naming it, and prints the versions
it used.

| File | What it does |
|---|---|
| `build-deps.sh` | Builds the third-party parts from pinned sources into a prefix (an hour on a GitHub runner, mostly Mesa and the SPIR-V translator), rewrites the shipped libraries' install names to `@rpath`, and checks them: arm64, built for macOS 26, needing only macOS's libraries and one another, no path of the build machine left in them. `--verify <prefix>` runs the checks alone (the CI does after restoring its cache). |
| `make-app.sh` | Assembles `Scacelith.app` from the build folder and the prefix (below), signs it ad hoc and checks that every library it loads resolves inside it. |
| `make-dmg.sh` | Makes the disk image with [dmgbuild](https://github.com/dmgbuild/dmgbuild) (`dmg-settings.py`: the window, the icons' places) and checks it: it verifies, mounts, and holds the signed app, the link to /Applications and the background. |
| `dmg_background.py` | Draws `res/macos/dmg-background.png` and its `@2x` from a render of the game (see "The disk image"). |
| `lib.sh` | Shell helpers of the three scripts. |
| `requirements-mesa.txt`, `requirements-dmgbuild.txt` | The Python packages of Mesa's build and of dmgbuild, pinned with the SHA-256 of their files (`pip --require-hashes --only-binary :all:`). |
| `mesa-patches/` | Five fixes of Zink and KosmicKrisp applied to Mesa (their origin is in its README). |

## The app

```
Scacelith.app/Contents/
  Info.plist                     res/macos/Info.plist.in, with the version of cmake/version.cmake
  MacOS/Scacelith                the game (build/scacelith), run path @executable_path/../Frameworks
  Frameworks/libEGL.1.dylib      Mesa's EGL, which the game loads (surfaceless: no window system)
  Frameworks/libgallium-26.2.4.dylib   Mesa's OpenGL with Zink, which libEGL links
  Frameworks/libvulkan.1.dylib   the Khronos Vulkan loader, which Zink loads as @rpath/libvulkan.1.dylib
  Frameworks/libvulkan_kosmickrisp.dylib   the Vulkan driver, which the loader loads
  Resources/vulkan/icd.d/kosmickrisp_icd.json   its manifest: library_path ../../../Frameworks/...
  Resources/Scacelith.icns       from res/icons/png/ (iconutil)
  Resources/LICENSE, licences/   as in the Windows and Linux archives, plus Mesa, the Vulkan loader,
                                 SPIRV-Tools, SPIRV-Headers and OpenSSL; macOS-dependencies.txt
                                 (build-deps.sh's PROVENANCE.txt: what was built from what)
```

At run time: the game opens `libEGL.1.dylib` from `Contents/Frameworks`; Zink, inside
libgallium, opens the Vulkan loader from `@rpath` (its run path is `@loader_path`); the loader
looks for drivers in the bundle's `Resources/vulkan/icd.d` before the system folders, and opens
the library its manifest names, relative to the manifest. A Mac with another Vulkan driver
installed system-wide (MoltenVK from the Vulkan SDK) lists it too: setting
`VK_LOADER_SEARCH_ONLY_IN_BUNDLE=1` (or `VK_DRIVER_FILES` to the bundle's manifest) before the
game loads libEGL keeps the loader to the bundled driver. `VK_LOADER_DEBUG=all` and
`MESA_DEBUG`/`ZINK_DEBUG` print what each layer does.

The app is signed ad hoc only (arm64 code must carry a signature to run; this one names no
developer) and is not notarized: macOS blocks its first launch, which the player allows in System
Settings > Privacy & Security > Open Anyway (or with `xattr -dr com.apple.quarantine
/Applications/Scacelith.app`). A Developer ID signature and notarization would need a paid Apple
developer account, its certificate and an App Store Connect key as repository secrets.

## Pins

Checked on 2026-10-10; updated by hand (Dependabot does not see them). The pins are at the top of
`build-deps.sh`; each Git tag is cloned and its commit compared with the pin, each download is
checked against its SHA-256.

| Part | Version | Pin | Shipped |
|---|---|---|---|
| Mesa | 26.2.4 (latest stable) | tag `mesa-26.2.4`, commit `96cb43121031992b85767f9c1be8f3f48e22b1d2`; fallback tarball `https://archive.mesa3d.org/mesa-26.2.4.tar.xz`, SHA-256 `bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9` (as in Homebrew's mesa formula) | yes, with `mesa-patches/` |
| Vulkan-Loader | 1.4.365 (latest) | tag `v1.4.365`, commit `f866657ff687a36d767cd7783bab800e31ebafaa` | yes |
| Vulkan-Headers | 1.4.365 | tag `v1.4.365`, commit `c46850864f4661461b0f6cb9922c058ffea4915e` | no (build) |
| OpenSSL | 3.6.5 | `openssl-3.6.5.tar.gz` from the GitHub release, SHA-256 `a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98` | linked into the game |
| SPIRV-Tools | SDK 1.4.363.0 | tag `vulkan-sdk-1.4.363.0`, commit `ef96ed763b43b59b33b31b362f09a02b729fa1c9` | compiled into Mesa (static) |
| SPIRV-Headers | SDK 1.4.363.0 | tag `vulkan-sdk-1.4.363.0`, commit `496543121ce6419f23d6fa5d7194ba66c36212d2` | no (build) |
| SPIRV-LLVM-Translator | 19.1.7 | tag `v19.1.7`, commit `3fac86cf760a4a2510f3c1e76001ea799812a2f6` | no (build) |
| LLVM and Clang | 19.1.x | Homebrew `llvm@19` | no (build) |
| MoltenVK headers | Homebrew `molten-vk` | Zink's build requires them on macOS | no (headers only) |
| Meson, Mako, PyYAML, packaging, MarkupSafe | 1.12.1, 1.4.3, 6.0.3, 26.3, 3.0.4 | `requirements-mesa.txt` (hashes) | no (build) |
| dmgbuild, ds-store, mac-alias | 1.6.7, 1.3.3, 2.2.3 | `requirements-dmgbuild.txt` (hashes) | no |

**Why LLVM 19.** LLVM only serves at build time: KosmicKrisp's internal kernels are OpenCL C,
which Mesa's `mesa_clc` compiles to SPIR-V through Clang, LLVM and the SPIR-V translator while
Mesa builds; the shipped libraries carry no LLVM (`-Ddraw-use-llvm=false`, no llvmpipe). LLVM 19
with the translator v19.1.7, linked statically, is the pairing known to build and run this stack:
the RecoilEngine-AppleSilicon port ships Mesa 26.2 with Zink and KosmicKrisp built that way, with
the patches of `mesa-patches/`. Homebrew's current `llvm` is built with link-time optimisation,
and its static libraries did not link `mesa_clc` there; the versioned `llvm@19` is a plain build.
Mesa's KosmicKrisp page recommends LLVM 20.1.8 or later (lower versions "may work but have not
been tested"); Homebrew's own Mesa 26.2.4 links LLVM 23 as a shared library, which the bundle
would then have to carry. The translator must match LLVM's major and minor version (Mesa checks).

**Updating.** Mesa: change the version, tag, commit and tarball hash, then run the build: a patch
that no longer applies stops it (drop it if upstream merged it). The loader: the tag and commit of
Vulkan-Loader and the same tag of Vulkan-Headers (`git ls-remote --tags`; an annotated tag's
commit is its `^{}` line). OpenSSL: the version and the SHA-256 of the new tarball (published next
to it as `.sha256`). LLVM: the formula, the major and the translator's tag and commit together.
The Python packages: the version and the hashes of every file PyPI lists for it.

## The CI and the release

`.github/workflows/ci.yml`'s macOS job (on `macos-26`) restores the prefix from the Actions cache,
keyed on `build-deps.sh`, `lib.sh`, the patches and `requirements-mesa.txt`, builds it when there
is none (and saves it at once, so that a failure further on does not lose the hour), checks it
with `--verify`, then builds the game, runs its tests, makes the app and the disk image (kept 14
days as a workflow artifact) and tries a headless screenshot with the bundled app on the runner's
virtual GPU (`--scene game --shot ... --start --no-intro`), whose failure is only reported for
now. The release workflow builds everything from scratch, the prefix included: nothing in the
attested disk image comes from a cache that another run wrote. Both macOS jobs are
`continue-on-error`: the port is experimental, and its failure holds back neither the other
platforms' checks nor their release (which then has no disk image).

## The disk image

`make-dmg.sh` runs dmgbuild with `dmg-settings.py`: an LZFSE-compressed read-only image (`ULFO`),
named "Scacelith <version>", whose window (660 x 400 points, no toolbar or sidebar) shows the app
on the left and a link to /Applications on the right, 128-point icons, the game's icon as the
volume's icon, and `res/macos/dmg-background.png` with its `@2x` (dmgbuild joins them into a
HiDPI TIFF with `tiffutil`).

The background is drawn by `dmg_background.py` from a render of the game (the robot across the
board, from the player's chair):

```sh
tools/shot.sh game render.png 8 1320x800 --start --no-intro --human white --mouse 0.5,0.03
python3 -I tools/macos/dmg_background.py render.png --preview /tmp/dmg-preview
```

It darkens and warms the render, writes "Scacelith" in Cinzel and the two lines of text in EB
Garamond (the game's fonts, SIL Open Font License), draws the arrow, and puts two frosted tiles
where Finder places the icons. Finder writes the icons' names in black in the light appearance
and in white in the dark one: the tiles are brought to an even luminance of about 0.18 under the
names, where both have a contrast ratio of about 4.5 (the script prints it). `--preview` writes
the window as Finder should show it, with stand-in icons, in both appearances. The icon positions
of `dmg-settings.py` and of the script go together.

## Not verified yet

Written without a Mac at hand: what only a `macos-26` run tells is the build of Mesa with these
options and LLVM 19 (a missing static library, a name the install-name rewriting does not
expect), the shipped libraries passing the checks, the app and the disk image on a real Finder
(the window's height: its bounds include the title bar, taken as 28 points), and whether
KosmicKrisp runs on the hosted runner's virtual GPU at all (the smoke run says).
