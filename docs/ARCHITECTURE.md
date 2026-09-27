# Scacelith architecture

In-house engine on OpenGL 4.6 core (DSA only), C++17, no third-party engine. The shipping target
is a single self-contained Windows x64 executable (MinGW-w64, static). Everything (shaders,
fonts, the Stockfish NNUE network) is embedded; only `Scacelith.ini` lives next to the exe.

## Build

```sh
# Linux dev/test build (X11 + GLX, used for unit tests and headless screenshots)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build
# Windows x64 exe (cross-compiled)
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release && ninja -C build-win
```

Headless screenshots (Xvfb + Mesa llvmpipe with `MESA_GL_VERSION_OVERRIDE=4.6`):
`tools/shot.sh <scene> out.png [frames] [WxH]` (Linux build) and `tools/shot_win.sh` (the real
Windows exe under wine). `scacelith --list-scenes` lists viewer scenes. Set
`SCACELITH_DUMP_SHADERS=<dir>` to dump preprocessed GLSL.

## Modules

| Dir | Role |
|---|---|
| `src/platform` | Win32 (+X11 for tests) window, GL 4.6 context, input, timing |
| `src/gl` | Generated GL 4.6 loader (`tools/gen_gl_loader.py`) |
| `src/math` | vec/mat/quat, reverse-Z projections, rays (conventions in `math.h`) |
| `src/core` | log, ini, embedded files, PNG writer |
| `src/render` | Renderer (forward PBR, shadows, planar reflections, probes), shader builder, meshes |
| `src/render/post` | Post chain (GTAO, SSR, volumetrics, TAA, motion blur, DOF, bloom, tonemap) |
| `src/render/materials` + `shaders/materials` | Material library (surface shaders + baked textures) |
| `src/scene` | Procedural geometry: hall, table, chairs, board, pieces, clock |
| `src/character` | Robot model + skeleton |
| `src/anim` | IK, hand tasks with fixed durations, gaze, idle |
| `src/chess` | Rules, clock, tournament arbiter (touch-move, illegal moves) |
| `src/ai` | Stockfish 16 in-process (UCI over in-memory streams), presets |
| `src/audio` | Procedural sound synthesis, mixer, reverb, WASAPI |
| `src/ui` | SDF text, widgets, menus |
| `src/game` | Game state machine, settings, world layout (`layout.h`) |
| `src/app` | Scene registry (`--scene`), test scenes |

## Rendering contracts

* World: meters, +Y up, board centred on the origin, see `src/game/layout.h`.
* Reverse-Z depth ([0,1], near = 1), `GL_GREATER`, `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`.
* Lighting values are pre-exposed (`frame.exposure.x`); the sun is in lux.
* Materials implement `void surface(in SurfaceInput i, inout Surface s)` (`shaders/include/surface.glsl`);
  the renderer lights them in every pass. Texture units and buffer bindings: `src/render/material.h`.
* FrameUBO (`shaders/include/common.glsl`) mirrors `render::FrameUBOData` — keep them in sync.
* Never use `std::cout`/`std::cin` in game code: they belong to the embedded Stockfish.
