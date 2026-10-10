#include "graphics_quality.h"
#include "../core/ini.h"
#include <algorithm>

namespace game {

namespace {

const char* const kOffOn[] = {"common.off", "common.on"};
const char* const kLowUltra[] = {"options.quality.low", "options.quality.medium", "options.quality.high",
                                 "options.quality.ultra"};
const char* const kOffUltra[] = {"common.off", "options.quality.low", "options.quality.medium", "options.quality.high",
                                 "options.quality.ultra"};

struct OptionInfo {
    const char* key;
    const char* const* labels;
    int count;
};
const OptionInfo kOptions[GfxOptionCount] = {
    {"shadows", kLowUltra, 4},
    {"reflections", kOffUltra, 5},
    {"ambient_occlusion", kOffUltra, 5},
    {"indirect_light", kLowUltra, 4},
    {"volumetric_light", kOffUltra, 5},
    {"tessellation", kOffOn, 2},
    {"antialiasing", kOffOn, 2},
    {"depth_of_field", kOffUltra, 5},
    {"motion_blur", kOffUltra, 5},
    {"bloom", kOffOn, 2},
};

// Levels of each preset, in GraphicsOption order. High and Ultra are the High and Ultra of the
// game before the presets were split into options (Ultra adds the wider shadow filter); Low and
// Medium are a little lighter than their old namesakes, and Very low turns everything off.
const GraphicsLevels kPresets[PresetCustom] = {
    //  shad refl  ao  ind  vol tess  aa  dof   mb bloom
    {{   0,   0,   0,   0,   0,   0,   0,   0,   0,   0}},  // Very low
    {{   1,   0,   1,   1,   0,   0,   1,   0,   0,   1}},  // Low
    {{   1,   1,   2,   1,   2,   0,   1,   0,   2,   1}},  // Medium
    {{   2,   3,   3,   2,   3,   1,   1,   3,   3,   1}},  // High
    {{   3,   4,   4,   3,   4,   1,   1,   4,   4,   1}},  // Ultra
};

// Sample-count row of PostFX (0 Low .. 3 Ultra) of an Off/Low..Ultra level (1..4).
int postLevel(int level) { return std::clamp(level - 1, 0, 3); }

}  // namespace

int graphicsLevelCount(int option) { return option >= 0 && option < GfxOptionCount ? kOptions[option].count : 1; }

const char* graphicsOptionKey(int option) { return option >= 0 && option < GfxOptionCount ? kOptions[option].key : ""; }

const char* graphicsLevelLabel(int option, int level) {
    if (option < 0 || option >= GfxOptionCount) return "";
    const OptionInfo& o = kOptions[option];
    return o.labels[std::clamp(level, 0, o.count - 1)];
}

GraphicsLevels presetLevels(int preset) {
    return kPresets[preset >= 0 && preset < PresetCustom ? preset : PresetHigh];
}

int matchingPreset(const GraphicsLevels& levels) {
    for (int p = 0; p < PresetCustom; ++p)
        if (kPresets[p] == levels) return p;
    return PresetCustom;
}

GraphicsLevels clampLevels(const GraphicsLevels& levels) {
    GraphicsLevels out = levels;
    for (int i = 0; i < GfxOptionCount; ++i) out[size_t(i)] = std::clamp(out[size_t(i)], 0, kOptions[i].count - 1);
    return out;
}

render::RenderSettings renderSettingsFor(const GraphicsLevels& in, float renderScale) {
    const GraphicsLevels g = clampLevels(in);
    auto at = [&g](GraphicsOption o) { return g[size_t(o)]; };
    render::RenderSettings r;
    r.quality = render::Quality::High;
    r.renderScale = std::clamp(renderScale, 0.5f, 2.0f);
    // Shadows: map size and cascades (the table, the area around it, the hall; with 2 the middle
    // one is left out), and the filter: plain 4-tap PCF, then PCSS soft shadows with more taps.
    static const int kShadowSize[4] = {1024, 2048, 4096, 4096};
    const int shadows = at(GfxShadows);
    r.shadowMapSize = kShadowSize[shadows];
    r.shadowCascades = shadows >= 2 ? 3 : 2;
    r.shadowFilter = shadows;
    // Reflections: the floor, table and board mirrors at a quarter, then half the render
    // resolution; from Medium the screen-space reflections too.
    const int refl = at(GfxReflections);
    r.planarReflections = refl >= 1;
    r.planarDivisor = refl == 1 ? 4 : 2;
    r.ssr = refl >= 2;
    r.ssrQuality = refl == 2 ? 0 : refl == 3 ? 2 : 3;
    r.ssao = at(GfxAmbientOcclusion) > 0;
    r.aoQuality = postLevel(at(GfxAmbientOcclusion));
    // Indirect light: Low lights the hall with a plain sky / ground ambient, the others bake the
    // light probes (finer and with more bounces from High).
    static const int kProbeRes[4] = {64, 64, 128, 128}, kBounces[4] = {1, 2, 2, 3};
    const int indirect = at(GfxIndirectLight);
    r.lightProbes = indirect > 0;
    r.probeResolution = kProbeRes[indirect];
    r.probeBounces = kBounces[indirect];
    r.volumetrics = at(GfxVolumetricLight) > 0;
    r.volumetricQuality = postLevel(at(GfxVolumetricLight));
    r.tessellation = at(GfxTessellation) > 0;
    r.taa = at(GfxAntiAliasing) > 0;
    r.dof = at(GfxDepthOfField) > 0;
    r.dofQuality = postLevel(at(GfxDepthOfField));
    r.motionBlur = at(GfxMotionBlur) > 0;
    r.motionBlurQuality = postLevel(at(GfxMotionBlur));
    r.bloom = at(GfxBloom) > 0;
    return r;
}

void readGraphicsSettings(const IniFile& ini, int& preset, GraphicsLevels& levels) {
    if (!ini.has("graphics.preset")) {
        // A file from before the presets: quality 0..3 was Low..Ultra, and the two switches could
        // turn off the motion blur and the depth of field of any of them.
        preset = std::clamp(ini.getInt("graphics.quality", 2), 0, 3) + PresetLow;
        levels = presetLevels(preset);
        bool changed = false;
        if (!ini.getBool("graphics.motion_blur", true) && levels[GfxMotionBlur] > 0) {
            levels[GfxMotionBlur] = 0;
            changed = true;
        }
        if (!ini.getBool("graphics.depth_of_field", true) && levels[GfxDepthOfField] > 0) {
            levels[GfxDepthOfField] = 0;
            changed = true;
        }
        if (changed) preset = PresetCustom;
        return;
    }
    preset = std::clamp(ini.getInt("graphics.preset", PresetHigh), 0, int(PresetCustom));
    if (preset != PresetCustom) {
        levels = presetLevels(preset);
        return;
    }
    const GraphicsLevels base = presetLevels(PresetHigh);
    for (int i = 0; i < GfxOptionCount; ++i)
        levels[size_t(i)] = ini.getInt(std::string("graphics_custom.") + kOptions[i].key, base[size_t(i)]);
    levels = clampLevels(levels);
}

void writeGraphicsSettings(IniFile& ini, int preset, const GraphicsLevels& in) {
    preset = std::clamp(preset, 0, int(PresetCustom));
    const GraphicsLevels levels = preset == PresetCustom ? clampLevels(in) : presetLevels(preset);
    ini.setInt("graphics.preset", preset);
    for (int i = 0; i < GfxOptionCount; ++i) ini.setInt(std::string("graphics_custom.") + kOptions[i].key, levels[size_t(i)]);
    // The keys of older versions: the nearest of their four presets (Very low and Custom: by the
    // shadows), and the two switches.
    int old = preset >= PresetLow && preset <= PresetUltra ? preset - PresetLow : std::max(0, levels[GfxShadows]);
    ini.setInt("graphics.quality", std::clamp(old, 0, 3));
    ini.setBool("graphics.motion_blur", levels[GfxMotionBlur] > 0);
    ini.setBool("graphics.depth_of_field", levels[GfxDepthOfField] > 0);
}

}  // namespace game
