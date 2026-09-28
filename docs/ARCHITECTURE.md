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
| `src/render` | Renderer (forward PBR, frame orchestration), shader builder, meshes |
| `src/render/lighting` + `shaders/lighting` | Atmosphere/sky, sun cascades (PCSS), light probes (GI), planar reflections |
| `src/render/post` | Post chain (GTAO, SSR, volumetrics, TAA, motion blur, DOF, bloom, tonemap) |
| `src/render/materials` + `shaders/materials` | Material library (surface shaders + baked textures) |
| `src/scene` | Procedural geometry: hall, table, chairs, board, pieces, clock |
| `src/character` | Robot model + skeleton |
| `src/anim` | IK, hand tasks with fixed durations, gaze, idle |
| `src/chess` | Rules, clock, tournament arbiter (touch-move, illegal moves) |
| `src/ai` | Stockfish 16 in-process (UCI over in-memory streams), presets |
| `src/audio` | Procedural sound synthesis, mixer, reverb, WASAPI |
| `src/ui` | SDF text (lazy atlas, font fallback, Arabic joining + bidi via `text_shape.h`), widgets (mirrored for RTL), menus |
| `src/i18n` + `assets/i18n` | Translations (`tr`, `trf`, `trn` with CLDR plurals), language choice, Unicode helpers (`unicode.h`: joining, bidi, line breaks) |
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

### Lighting (render-lighting: `src/render/lighting`, `shaders/lighting`, `shaders/include/lighting.glsl`)

Frame order in `Renderer::endFrame`: upload draws/lights, sort → atmosphere LUTs + sky cubemap
capture (only when the sun / sky changed) → sun cascades → light probe bake (when dirty) → planar
reflections → prepass → `PostFX::computeAO` → opaque → sky → transparents → `PostFX::resolve`.

**Units / exposure.** Photometric: sun in lux, sky and emission in nits, point lights in candela.
Default `Environment::exposureEV100` is 12.3 (daylight interior with the board in the sun; was
11.5). With `Environment::physicalSky` (default on) the sun colour and illuminance come from the
atmosphere (Hillaire 2020, 128 klux at the top of the atmosphere, reddened near the horizon) and
`sunColor` / `sunIlluminance` / `skyIlluminance` are ignored; `sunIntensityScale`,
`skyIntensity`, `cloudCoverage`, `sunSoftness` (PCSS penumbra scale), `ambientIntensity` (all IBL)
and `altitudeKm` are the artistic controls. Default `sunDirection` = the hall's recommended sun
`normalize(-0.70, 0.52, 0.49)`. `FrameUBO.sunDirection.w` = sun angular radius (0.00465 rad),
`sunRadiance` = pre-exposed illuminance (rgb) + raw lux (w). `ambientSky` / `ambientGround` are
only a fallback (probes off or not baked yet).

**LightingUBO** (`UBO_LIGHTING` = 2, `shaders/lighting/lighting_ubo.glsl` ↔
`render::LightingUBOData` in `src/render/lighting/lighting_data.h`, std140, bound for every pass
including post): probe positions / radii / parallax boxes, L2 SH irradiance of each probe and of the
sky (cosine-convolved, divided by π, windowed), per-cascade shadow scales (`shadowScale[c]` = xy
metres per shadow uv, z metres per depth unit, w texel size), sun (`sunParams`, `sunTOA`), sky
(`skyParams2`: cloud coverage, time, Mie scale, sky intensity), planar info (enabled, max lod,
size) and misc (specular AA strength, cascade blend band, ambient intensity). Probe data are
pre-exposed with the exposure of the bake: multiply by `probeInfo.z` (the lighting code does it).
Helpers: `shEvalProbe(k * 9, n)`, `shEvalSky(n)`.

**Texture units** (material.h numbering, bound by `Renderer::bindGlobalTextures`):
`TEXUNIT_SHADOW` / `TEXUNIT_SHADOW_DEPTH` = D16 2D array, one layer per cascade (compare / raw);
`TEXUNIT_IRRADIANCE` is not used (SH lives in the LightingUBO; a dummy stays bound);
`TEXUNIT_SPECULAR` = samplerCubeArray, layer = probe index, GGX prefiltered, lod = roughness ×
`probeInfo.y`; `TEXUNIT_PLANAR` = RGBA16F 2D array (one layer per reflector, half res, Gaussian mip
chain; alpha = distance from the mirror plane to the reflected surface, 1000 for the sky);
`TEXUNIT_BRDF_LUT` = RGBA16F 128²: r,g = split-sum DFG (A, B) indexed by (NoV, perceptual
roughness), b = Charlie sheen directional albedo; `TEXUNIT_SKY` = RGBA16F cube (64², full mips):
pre-exposed sky radiance without the sun disk (clouds included). `Renderer::setGlobalTexture(slot,
tex)` binds `TEXUNIT_GLOBAL0 + slot` (0..4) every pass.

**Sun shadows.** `RenderSettings::shadowCascades` (2 or 3) cascades fitted to fixed receiver
regions (`Renderer::setShadowRegions`, finest first; default: table + seated players, the area
around the table, the whole hall incl. walls) — not to the view frustum. The depth range of every
cascade covers `Renderer::setSceneBounds` (default: hall + thick walls) and depth clamp is on, so
casters between the sun and the table (window walls) are never lost. `DRAW_STATIC` casters are
cached per cascade (`staticShadowCache`, re-rendered on `invalidateStatic()` / sun move), dynamic
casters are drawn on top every frame; small dynamic items (pieces) are skipped in the coarse
cascades inside the first region. Filtering: PCSS (physical sun radius × `sunSoftness`, blocker
search with textureGather) with normal-offset + receiver-plane depth bias; cascade borders are
dithered (TAA resolves them). `evalSunShadow()` also returns the matter thickness towards the sun.

**Light probes.** `Renderer::setLightProbes` (≤ 16 `LightProbeDesc`; default: a priority probe
above the table + a 3×4 grid at 1.8 m + 3 high probes). Probes capture **`DRAW_STATIC` geometry
only** with `PassId::Probe` (no tessellation, `DRAW_NO_REFLECTION` skipped, sky drawn), 2 bounces
(`probeBounces`; bounce 0 has no ambient), `probeResolution`² faces. They re-bake at startup, on
`invalidateStatic()`, when the sun moves by more than 1° or the sky changes (≈ 1.3 s for 16 probes
× 2 bounces at 128² on llvmpipe). Shading blends priority probes first, then the grid with
normalised radial kernels (continuous everywhere), box-projected specular from the two strongest.

**Planar reflections.** `PlanarReflector` gains `bounds` (skip when off screen + scissor to its
screen rectangle) and `minObjectSize` (skip objects whose radius / distance is smaller in the
reflection). Materials opt in with `Material::planarReflector`; the renderer samples the layer with
the reflected-lobe footprint (roughness + distance to the reflected object), normal-map distortion
and an edge fade, and falls back to probes. Reflections skip `DRAW_NO_REFLECTION` and
`DRAW_HIDDEN_MAIN` items.

**Surface / BRDF.** Every `Surface` field is honoured: clear coat (with base F0 correction and
separate IBL lobe), sheen (Charlie + DFG-LUT energy scaling, IBL term), anisotropy (GGX aniso +
bent reflection vector), subsurface (energy-conserving coloured wrap + Beer–Lambert transmittance
through the shadow-map thickness, `subsurfaceRadius` = mean free path), thin translucency
(`thickness` > 0), multi-scatter energy compensation, specular occlusion from `occlusion` × AO,
horizon fade, geometric specular AA (`RenderSettings::specularAA`). `MATERIAL_ALPHA_TEST` materials
are alpha tested in the prepass and the shadow pass too.

**Transparents.** Rendered into `Targets::fbTransparent` (HDR colour + depth, no MRT) with
dual-source blending `dst = src0 + dst × src1`. Glass contract (`s.transmission > 0`): the colour
output is the full shading (specular reflection never scaled by alpha; diffuse weighted by
1 − transmission), the background is multiplied by `transmission × (1 − F)² × albedo`
(`transmittanceOf()`). Without transmission, plain coverage blending by `s.alpha`.

**Lights.** `PointLight` (64 bytes, mirrors `PointLightData`) gains `direction`, `spotCosOuter`,
`spotCosInner` (spot when `spotCosOuter > -1`, use `setSpot(dir, inner, outer)`), and
`sourceRadius` (sphere light highlight widening). Omni lights keep the old defaults. No point light
shadows.

**Other additive API.** `DRAW_NO_CULL` (items are otherwise frustum culled by their bounding
sphere in every pass), `DrawFilter` + `Renderer::drawScene(pass, transparents, filter)`,
`Renderer::renderSky()` (public), `skyCubemap()`, `specularProbes()`, `lightingUBO()`, `brdfLut()`,
`environment()`, `RenderSettings::{shadowCascades, staticShadowCache, lightProbes,
probeResolution, probeBounces, specularAA}` (set by the quality presets). GLSL: `sq(vec2/vec3)`,
`F_Schlick(vec3 f0, vec3 f90, float)`, `gtaoMultiBounce()`, `specularOcclusion()`; the baseline
helpers `evalDirect()`, `ambientSpecular()`, `sunShadow()`, `hemisphereAmbient()` still exist.
`SCACELITH_GPU_PROFILE=1` logs per-pass CPU+glFinish timings each frame (opt-in, stalls the GPU).
Test scenes: `lightbox` (hall of boxes per layout.h: `--view 0..4`, `--sun az,el`, `--ev`) and
`testbed` (outdoor material spheres: `--view 0..2`, `--dusk`, `--sun`, `--ev`).
