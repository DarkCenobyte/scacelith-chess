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

**What builds and runs**
- Stockfish: two variants, `armv8` (NEON, `-march=armv8-a`) and `armv8-dotprod`
  (`-march=armv8.2-a+dotprod`, NEON dot product: Cortex-A55/A75 and later, Neoverse N1, Apple M1),
  isolated as on x86-64 (`cmake/isolate.cmake` is ELF-generic). `scacelith/cpu.cpp` takes the second
  when `getauxval(AT_HWCAP) & HWCAP_ASIMDDP`; `engine.arch` names them. The instruction-set audit
  has an aarch64 mode (`tools/isa_audit.py --arch aarch64`, see
  `third_party/stockfish/README.scacelith.md`).
- TTS: a NEON kernel table (`src/tts/kernels_neon.cpp`, the shared kernels of `kernels_impl.h`) next
  to the scalar reference (`kernels_scalar.h`, the same as on x86-64). NEON is part of ARMv8-A, so
  there is nothing to check at run time and no TTS instruction-set audit; `tts.arch` takes `scalar`
  or `neon`, and the worker threads set FPCR.FZ where x86-64 sets MXCSR's FTZ and DAZ.
- Audio: `DenormalGuard` (`src/audio/dsp.h`) sets FPCR.FZ (bit 24) with `mrs`/`msr fpcr`.
- `fmadd` contraction: every float comparison of the tests has a tolerance, nothing needed
  `-ffp-contract=off`. Unsigned `char`: nothing found that depends on its sign.
- Tests under `qemu-aarch64` (Release, no voice model): everything passes but five tests that
  measure time or rely on what qemu-user does not emulate: `audio_mixer_cpu_cost`,
  `pgn_long_lines_take_linear_time` and `game_review_engine_scholars_mate` (time limits),
  `audio_live_engine_init_shutdown` (a bank refresh expected within a delay) and
  `net_sigpipe_spawned_program_default` (qemu's `posix_spawn` reports no failure to execute a
  missing program). The CI runs them all natively.
- The release build adds no `-march`: GCC 11's default `-march=armv8-a` with `-moutline-atomics`
  (libgcc's atomics take LSE at run time; `-static-libstdc++ -static-libgcc` link libgcc's helpers
  in). A cross build against Ubuntu 24.04's arm64 glibc (2.39) needs `GLIBC_2.38` symbols: only a
  build on 22.04 keeps the 2.34 floor.

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
- Zink announces OpenGL 4.6 there only because the game forces it (`MESA_GL_VERSION_OVERRIDE`):
  Apple's GPUs have no geometry shaders, transform feedback or double precision (unused by the
  game), no depth clamping, and KosmicKrisp before Mesa 26.2 no tessellation. The engine reads
  those features from the extension list (`gl46::caps()`, `src/gl/gl_context.h`): without
  tessellation it is off whatever the preset and greyed out in Options > Graphics; without depth
  clamping the sun shadow cascades are stretched to the nearest caster.
- The platform layer (window, input, high DPI) and the audio backend are macOS-specific code.
- Stockfish is one variant, `apple-silicon` (the `armv8-dotprod` flags: every Apple Silicon CPU has
  the dot product), built with Apple clang and not isolated (the isolation needs GNU binutils and
  ELF or PE; with one variant nothing can leak, and its static initialisers run at program start),
  so without the audit. The embedded network (INCBIN) and the embedded files (`cmake/embed.cmake`)
  go in Mach-O sections; the TTS uses the NEON table and FPCR.FZ as on Linux aarch64, its workers'
  lower priority through their QoS class (`pthread_set_qos_class_self_np`, as `setpriority`
  applies to the whole process on macOS).
- OpenSSL is not part of macOS: `build-deps.sh` builds a static OpenSSL 3.6.5 whose CA store is
  macOS's `/etc/ssl/cert.pem`.
- The app is signed ad hoc and not notarized: macOS blocks its first launch until the player allows
  it (System Settings > Privacy & Security > Open Anyway). Developer ID signing and notarization
  need a paid Apple account and repository secrets.
- The `.icns` is made by `tools/macos/make-app.sh` from `res/icons/png/` with `iconutil -c icns`
  (iconset: `icon_16x16` = 16, `@2x` = 32, `icon_32x32` = 32, `@2x` = 64, `icon_128x128` = 128,
  `@2x` = 256, `icon_256x256` = 256, `@2x` = 512, `icon_512x512` = 512, `@2x` = 1024).
