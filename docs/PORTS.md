# Ports: AppImage, Linux aarch64, macOS

**Status.** Linux aarch64 and macOS on Apple Silicon are built by the CI and the release workflow
(see the README); the macOS build is experimental, and no AppImage is made yet. This document keeps
what was found while preparing these targets, so that the remaining work can start from it.

## AppImage (x86-64, later aarch64)

**Layout of the AppDir**
- `AppRun`: a symlink to `usr/bin/scacelith`.
- `scacelith.desktop` (the same file as `res/linux/scacelith.desktop`) and `scacelith.png` (the
  256 px icon) at the root; appimagetool makes `.DirIcon` a symlink to the root icon.
- `usr/share/applications/scacelith.desktop`, `usr/share/icons/hicolor/<N>x<N>/apps/scacelith.png`
  (the sizes of `res/icons/png/`), `usr/share/doc/scacelith/` (LICENSE and the licences folder of
  the archive).
- An AppStream metainfo file is optional (appimagetool only warns).

**Building it**
- `ARCH=x86_64 appimagetool --appimage-extract-and-run --no-appstream --runtime-file runtime AppDir
  Scacelith-<version>-linux-x86_64.AppImage`. With `--runtime-file` it works offline and the output
  is byte-identical from one run to the next (tested: 94.6 MB for the 1.0.0-beta.1 binary).
- Without `--runtime-file`, appimagetool downloads the "continuous" runtime at build time: pin it.
- Pins checked on 2026-10-04 (to be updated by hand, Dependabot does not see them):

| File | URL | SHA-256 |
|---|---|---|
| appimagetool 1.9.1 x86_64 | `https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage` | `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0` |
| appimagetool 1.9.1 aarch64 | `…/1.9.1/appimagetool-aarch64.AppImage` | `f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158` |
| type2-runtime 20251108 x86_64 | `https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64` | `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d` |
| type2-runtime 20251108 aarch64 | `…/20251108/runtime-aarch64` | `00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444` |

**Behaviour to keep in mind**
- The static type2 runtime needs no libfuse2, only `fusermount`/`fusermount3`. Without them it
  prints "No suitable fusermount binary found" and still runs; `--appimage-extract-and-run` or
  `APPIMAGE_EXTRACT_AND_RUN=1` work too. The runtime sets `APPIMAGE` and `APPDIR`.
- Never bundle libssl (a bundled copy looks for its CA store at the build distribution's path and
  breaks TLS on Fedora, Arch, openSUSE...), libasound (dlopen'd: the host's library, configuration
  and PulseAudio/PipeWire plugins must be used), libsecret (dlopen'd: it talks to the host's
  keyring over D-Bus), libGL or libX11.
- The game keeps its settings, log and saved logins in `$XDG_CONFIG_HOME/scacelith/` (by default
  `~/.config/scacelith/`) on Linux unless a `Scacelith.ini` stands next to the executable (portable
  mode): in extract-and-run mode the executable lives in `$TMPDIR/appimage_extracted_<hash>/usr/bin`, so the portable mode must never
  apply to an AppImage.

## Linux aarch64

**Build and test environment**
- The CI builds and tests it on `ubuntu-24.04-arm` (the `linux-aarch64` job of `ci.yml`), the
  release on `ubuntu-22.04-arm` (GCC 11, binutils 2.38, OpenSSL 3.0.2: the same glibc 2.34 floor
  as the x86-64 archive), both free GitHub runners for public repositories. The release archive is
  `Scacelith-<version>-linux-aarch64.tar.gz`, laid out as the x86-64 one.
- Locally, cross-compile with `cmake/aarch64-linux-gnu.cmake` (Debian/Ubuntu multiarch: `dpkg
  --add-architecture arm64`, the arm64 sources from ports.ubuntu.com, `g++-aarch64-linux-gnu`,
  `libssl-dev:arm64 libx11-dev:arm64 libgl-dev:arm64`) and run the tests under `qemu-aarch64 -L
  /usr/aarch64-linux-gnu` (`qemu-user`). The toolchain file sets `CMAKE_CROSSCOMPILING_EMULATOR`.
- OpenGL 4.6 on aarch64 means NVIDIA (Jetson Orin, discrete GPUs), AMD discrete GPUs or Asahi Linux
  on Apple M1/M2. A Raspberry Pi 4/5 (V3D, desktop GL 3.1) cannot run the game.

**What differs from x86-64** (the points the port had to handle)
- Stockfish: `SF_ALL_VARIANTS` (`third_party/stockfish/CMakeLists.txt`) and the `SF_ISA_*` flags
  were x86-64 only (`-msse2`, `-mavx2`...). Stockfish 19 has ARM targets (armv8: NEON;
  armv8-dotprod: `-march=armv8.2-a+dotprod`, NEON dot product); a runtime choice between them
  reads `getauxval(AT_HWCAP) & HWCAP_ASIMDDP`. The variant isolation (`cmake/isolate.cmake`) is
  ELF-generic; the instruction-set audit (`tools/isa_audit.py`) is x86 only.
- TTS: the kernels are SSE2/AVX2/AVX-VNNI/AVX-512 (`src/tts/kernels_*.cpp`), the dispatch reads
  cpuid (`src/tts/cpu.cpp`) and `src/tts/threads.cpp` used `_mm_getcsr`: aarch64 needs a NEON or
  portable kernel set and its own dispatch; the TTS instruction-set audit in `CMakeLists.txt` is
  x86 only.
- Audio: `DenormalGuard` (`src/audio/dsp.h`) did nothing on aarch64, where it must set FPCR.FZ
  (bit 24) with `mrs`/`msr fpcr` (the audio tests assert that no denormal reaches the output).
- GCC contracts `a*b+c` into `fmadd` by default on aarch64: results are not bitwise identical to
  x86-64; tests with tolerances are fine, bit-exact ones may need `-ffp-contract=off`.
- `char` is unsigned on aarch64 Linux: code must not assume it is signed.

## macOS (Apple Silicon, experimental)

The release's `Scacelith-<version>-macos-arm64-experimental.dmg` holds Scacelith.app for Apple
Silicon Macs with macOS 26 or later. The scripts that make it, their pins and how the app is laid
out are in [`tools/macos/README.md`](../tools/macos/README.md). What shaped it:
- macOS caps OpenGL at 4.1 core, while the renderer needs `#version 460 core`, compute shaders
  (post-processing, probes), direct state access, SSBOs, image load/store, `glClipControl`, texture
  views and multi-draw indirect. Rather than a Metal renderer, the app carries Mesa's OpenGL 4.6:
  Zink (OpenGL on Vulkan) on KosmicKrisp, Mesa's Vulkan driver on Metal 4 (hence macOS 26 and
  Apple GPUs only), loaded through Mesa's EGL (surfaceless) with the Khronos Vulkan loader, all
  built from source by `tools/macos/build-deps.sh` (Mesa 26.2.4 and five patches). Intel Macs are
  not supported.
- The platform layer (window, input, high DPI) and the audio backend are macOS-specific code.
- The Stockfish variant isolation was written for ELF and PE with GNU binutils; macOS uses Mach-O
  and Apple's linker. OpenSSL is not part of macOS: `build-deps.sh` builds a static OpenSSL 3.6.5
  whose CA store is macOS's `/etc/ssl/cert.pem`.
- The app is signed ad hoc and not notarized: macOS blocks its first launch until the player allows
  it (System Settings > Privacy & Security > Open Anyway). Developer ID signing and notarization
  need a paid Apple account and repository secrets.
- The `.icns` is made by `tools/macos/make-app.sh` from `res/icons/png/` with `iconutil -c icns`
  (iconset: `icon_16x16` = 16, `@2x` = 32, `icon_32x32` = 32, `@2x` = 64, `icon_128x128` = 128,
  `@2x` = 256, `icon_256x256` = 256, `@2x` = 512, `icon_512x512` = 512, `@2x` = 1024).
