// Options > Graphics (game/graphics_quality.h): the presets, the render settings they give and the
// [graphics] keys of Scacelith.ini, old files included.
#include "test.h"
#include "core/embedded.h"
#include "core/ini.h"
#include "game/graphics_quality.h"
#include "i18n/i18n.h"
#include <set>

using namespace game;

// Very low puts every option at its lowest level, Ultra at its highest, and each preset in between
// is at least the one before it on every option.
TEST(graphics_presets_span_the_levels) {
    const GraphicsLevels veryLow = presetLevels(PresetVeryLow), ultra = presetLevels(PresetUltra);
    for (int o = 0; o < GfxOptionCount; ++o) {
        CHECK(graphicsLevelCount(o) >= 2);
        CHECK_EQ(veryLow[size_t(o)], 0);
        CHECK_EQ(ultra[size_t(o)], graphicsLevelCount(o) - 1);
    }
    for (int p = PresetVeryLow + 1; p < PresetCustom; ++p) {
        const GraphicsLevels lo = presetLevels(p - 1), hi = presetLevels(p);
        CHECK(lo != hi);
        for (int o = 0; o < GfxOptionCount; ++o) CHECK(lo[size_t(o)] <= hi[size_t(o)]);
    }
    for (int p = 0; p < PresetCustom; ++p) {
        CHECK_EQ(matchingPreset(presetLevels(p)), p);
        CHECK(clampLevels(presetLevels(p)) == presetLevels(p));
    }
    GraphicsLevels mix = presetLevels(PresetHigh);
    mix[GfxMotionBlur] = 0;
    CHECK_EQ(matchingPreset(mix), int(PresetCustom));
    CHECK(presetLevels(PresetCustom) == presetLevels(PresetHigh));
}

// What the renderer gets: Very low turns every effect off, High is the High of the game before
// the presets were split into options, and no render setting goes down from one preset to the next.
TEST(graphics_presets_render_settings) {
    const render::RenderSettings v = renderSettingsFor(presetLevels(PresetVeryLow), 1.0f);
    CHECK_EQ(v.shadowMapSize, 1024);
    CHECK_EQ(v.shadowCascades, 2);
    CHECK_EQ(v.shadowFilter, 0);
    CHECK(!v.planarReflections && !v.ssr && !v.ssao && !v.volumetrics && !v.tessellation && !v.taa && !v.dof &&
          !v.motionBlur && !v.bloom && !v.lightProbes);

    const render::RenderSettings h = renderSettingsFor(presetLevels(PresetHigh), 1.0f);
    CHECK_EQ(h.shadowMapSize, 4096);
    CHECK_EQ(h.shadowCascades, 3);
    CHECK_EQ(h.shadowFilter, 2);
    CHECK(h.planarReflections && h.ssr && h.ssao && h.volumetrics && h.tessellation && h.taa && h.dof && h.motionBlur &&
          h.bloom && h.lightProbes);
    CHECK_EQ(h.planarDivisor, 2);
    CHECK_EQ(h.probeResolution, 128);
    CHECK_EQ(h.probeBounces, 2);
    CHECK_EQ(h.aoQuality, 2);
    CHECK_EQ(h.ssrQuality, 2);
    CHECK_EQ(h.volumetricQuality, 2);
    CHECK_EQ(h.dofQuality, 2);
    CHECK_EQ(h.motionBlurQuality, 2);

    const render::RenderSettings u = renderSettingsFor(presetLevels(PresetUltra), 1.0f);
    CHECK_EQ(u.shadowFilter, 3);
    CHECK_EQ(u.probeBounces, 3);
    CHECK(u.aoQuality == 3 && u.ssrQuality == 3 && u.volumetricQuality == 3 && u.dofQuality == 3 && u.motionBlurQuality == 3);

    // A sample row only counts while its effect is on.
    auto row = [](bool on, int q) { return on ? q : -1; };
    for (int p = PresetVeryLow + 1; p < PresetCustom; ++p) {
        const render::RenderSettings a = renderSettingsFor(presetLevels(p - 1), 1.0f);
        const render::RenderSettings b = renderSettingsFor(presetLevels(p), 1.0f);
        CHECK(a.shadowMapSize <= b.shadowMapSize && a.shadowCascades <= b.shadowCascades && a.shadowFilter <= b.shadowFilter);
        CHECK(int(a.planarReflections) <= int(b.planarReflections));
        CHECK(!a.planarReflections || a.planarDivisor >= b.planarDivisor);
        CHECK(int(a.lightProbes) <= int(b.lightProbes) && a.probeResolution <= b.probeResolution && a.probeBounces <= b.probeBounces);
        CHECK(row(a.ssr, a.ssrQuality) <= row(b.ssr, b.ssrQuality));
        CHECK(row(a.ssao, a.aoQuality) <= row(b.ssao, b.aoQuality));
        CHECK(row(a.volumetrics, a.volumetricQuality) <= row(b.volumetrics, b.volumetricQuality));
        CHECK(row(a.dof, a.dofQuality) <= row(b.dof, b.dofQuality));
        CHECK(row(a.motionBlur, a.motionBlurQuality) <= row(b.motionBlur, b.motionBlurQuality));
        CHECK(int(a.taa) <= int(b.taa) && int(a.bloom) <= int(b.bloom) && int(a.tessellation) <= int(b.tessellation));
    }
    // The render scale of Options > Display passes through, clamped.
    CHECK(renderSettingsFor(presetLevels(PresetLow), 0.75f).renderScale == 0.75f);
    CHECK(renderSettingsFor(presetLevels(PresetLow), 9.0f).renderScale == 2.0f);
    CHECK(renderSettingsFor(presetLevels(PresetLow), 0.1f).renderScale == 0.5f);
}

// Out-of-range levels (a hand-edited file) are clamped to each option's range.
TEST(graphics_levels_clamped) {
    GraphicsLevels l;
    l.fill(99);
    CHECK(clampLevels(l) == presetLevels(PresetUltra));
    l.fill(-5);
    CHECK(clampLevels(l) == presetLevels(PresetVeryLow));
    CHECK(renderSettingsFor(l, 1.0f).shadowMapSize == 1024);
}

// Files written before the presets: graphics.quality 0..3 (Low..Ultra) and the motion blur and
// depth of field switches.
TEST(graphics_settings_from_old_files) {
    int preset = -1;
    GraphicsLevels levels{};
    {
        IniFile ini;  // no [graphics] at all: High, the old default
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(preset, int(PresetHigh));
        CHECK(levels == presetLevels(PresetHigh));
    }
    for (int q = 0; q < 4; ++q) {
        IniFile ini;
        ini.setInt("graphics.quality", q);
        ini.setBool("graphics.motion_blur", true);
        ini.setBool("graphics.depth_of_field", true);
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(preset, PresetLow + q);
        CHECK(levels == presetLevels(PresetLow + q));
    }
    {
        // High without motion blur: Custom, High's levels but the motion blur off.
        IniFile ini;
        ini.setInt("graphics.quality", 2);
        ini.setBool("graphics.motion_blur", false);
        ini.setBool("graphics.depth_of_field", true);
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(preset, int(PresetCustom));
        GraphicsLevels want = presetLevels(PresetHigh);
        want[GfxMotionBlur] = 0;
        CHECK(levels == want);
    }
    {
        // Ultra without either: both off.
        IniFile ini;
        ini.setInt("graphics.quality", 3);
        ini.setBool("graphics.motion_blur", false);
        ini.setBool("graphics.depth_of_field", false);
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(preset, int(PresetCustom));
        CHECK_EQ(levels[GfxMotionBlur], 0);
        CHECK_EQ(levels[GfxDepthOfField], 0);
        CHECK_EQ(levels[GfxShadows], presetLevels(PresetUltra)[GfxShadows]);
    }
    {
        // Low without depth of field: Low has none anyway, it stays Low.
        IniFile ini;
        ini.setInt("graphics.quality", 0);
        ini.setBool("graphics.depth_of_field", false);
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(preset, int(PresetLow));
    }
    {
        IniFile ini;  // out of range
        ini.setInt("graphics.quality", 7);
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(preset, int(PresetUltra));
    }
}

// The preset and the custom levels survive a save and a load; a preset other than Custom gives its
// own levels whatever [graphics_custom] holds; the old keys are written for older versions.
TEST(graphics_settings_round_trip) {
    int preset = -1;
    GraphicsLevels levels{};
    GraphicsLevels custom = presetLevels(PresetMedium);
    custom[GfxShadows] = 3;
    custom[GfxBloom] = 0;
    {
        IniFile ini;
        writeGraphicsSettings(ini, PresetCustom, custom);
        CHECK_EQ(ini.getInt("graphics.preset", -1), int(PresetCustom));
        CHECK_EQ(ini.getInt("graphics_custom.shadows", -1), 3);
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(preset, int(PresetCustom));
        CHECK(levels == custom);
        // Older versions read a quality by the shadows, and the switches.
        CHECK_EQ(ini.getInt("graphics.quality", -1), 3);
        CHECK(ini.getBool("graphics.motion_blur", false) == (custom[GfxMotionBlur] > 0));
        CHECK(!ini.getBool("graphics.depth_of_field", true));
    }
    for (int p = 0; p < PresetCustom; ++p) {
        IniFile ini;
        writeGraphicsSettings(ini, p, custom);  // stale levels: the preset wins
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(preset, p);
        CHECK(levels == presetLevels(p));
        CHECK_EQ(ini.getInt("graphics.quality", -1), p == PresetVeryLow ? 0 : p - PresetLow);
    }
    {
        IniFile ini;
        ini.setInt("graphics.preset", 42);
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(preset, int(PresetCustom));
        CHECK(levels == presetLevels(PresetHigh));  // no [graphics_custom]: High's levels
    }
    {
        IniFile ini;
        ini.setInt("graphics.preset", PresetCustom);
        ini.setInt("graphics_custom.reflections", 50);
        ini.setInt("graphics_custom.bloom", -3);
        readGraphicsSettings(ini, preset, levels);
        CHECK_EQ(levels[GfxReflections], graphicsLevelCount(GfxReflections) - 1);
        CHECK_EQ(levels[GfxBloom], 0);
    }
}

// Every option and level shown in Options > Graphics has its English text (the other languages
// are checked against English by i18n_lang_files_match_english), and the keys are distinct.
TEST(graphics_options_translated) {
    std::vector<std::pair<std::string, std::string>> en;
    REQUIRE(i18n::parse(embedded::text("assets/i18n/en.lang"), en, nullptr));
    std::set<std::string> have;
    for (auto& kv : en) have.insert(kv.first);
    std::set<std::string> keys;
    for (int o = 0; o < GfxOptionCount; ++o) {
        const std::string key = graphicsOptionKey(o);
        CHECK(!key.empty());
        CHECK(keys.insert(key).second);
        CHECK(have.count("options." + key) == 1);
        CHECK(have.count("options." + key + ".help") == 1);
        for (int l = 0; l < graphicsLevelCount(o); ++l) CHECK(have.count(graphicsLevelLabel(o, l)) == 1);
    }
    for (const char* k : {"options.quality", "options.quality.help", "options.quality.very_low", "options.quality.custom"})
        CHECK(have.count(k) == 1);
}
