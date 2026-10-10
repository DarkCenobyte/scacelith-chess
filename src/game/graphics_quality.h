// Options > Graphics: the settings the player can set one by one (shadows, reflections, ambient
// occlusion...), the presets that set them all at once (Very low .. Ultra, plus Custom for the
// player's own mix), their [graphics] keys in Scacelith.ini and the render settings they give.
// Engine-free and GL-free (it only fills render::RenderSettings): in the core library, so that the
// unit tests check the presets and the reading of old settings files.
#pragma once
#include "../render/renderer.h"
#include <array>

class IniFile;

namespace game {

// One setting of Options > Graphics. Its level runs from 0, the cheapest (the effect off when it
// can be), to graphicsLevelCount() - 1, the most demanding.
enum GraphicsOption {
    GfxShadows,           // sun shadows: 0 Low (1024 px, 2 cascades, plain filtering) .. 3 Ultra
    GfxReflections,       // mirror reflections of the floor, table and board, then screen-space ones on
                          //   everything glossy: 0 Off, 1 Low (mirrors at quarter resolution) .. 4 Ultra
    GfxAmbientOcclusion,  // 0 Off, 1 Low .. 4 Ultra
    GfxIndirectLight,     // light bouncing around the hall: 0 Low (a plain sky/ground ambient) .. 3 Ultra
    GfxVolumetricLight,   // sun shafts and dust motes: 0 Off, 1 Low .. 4 Ultra
    GfxTessellation,      // smooth silhouettes of the pieces: 0 Off, 1 On
    GfxAntiAliasing,      // temporal anti-aliasing: 0 Off, 1 On
    GfxDepthOfField,      // 0 Off, 1 Low .. 4 Ultra
    GfxMotionBlur,        // 0 Off, 1 Low .. 4 Ultra
    GfxBloom,             // glow around the brightest highlights: 0 Off, 1 On
    GfxOptionCount
};

// The levels of every option, indexed by GraphicsOption.
using GraphicsLevels = std::array<int, GfxOptionCount>;

// Presets, in Options > Graphics order. Very low sets every option to its lowest level and Ultra
// to its highest; Custom is the player's own levels.
enum GraphicsPreset { PresetVeryLow, PresetLow, PresetMedium, PresetHigh, PresetUltra, PresetCustom, GraphicsPresetCount };

// Number of levels of an option (2 for the on/off ones).
int graphicsLevelCount(int option);
// Key of an option: its key in the [graphics_custom] section of Scacelith.ini and its name in the
// translations (options.<key>, options.<key>.help).
const char* graphicsOptionKey(int option);
// Translation keys of an option's levels, lowest first ("common.off", "options.quality.low"...).
const char* graphicsLevelLabel(int option, int level);

// The levels of a preset (PresetCustom: High's).
GraphicsLevels presetLevels(int preset);
// The preset whose levels these are, PresetCustom when none.
int matchingPreset(const GraphicsLevels& levels);
// Every level clamped to its option's range.
GraphicsLevels clampLevels(const GraphicsLevels& levels);

// Render settings for these levels (renderScale: Options > Display, clamped to 0.5..2).
render::RenderSettings renderSettingsFor(const GraphicsLevels& levels, float renderScale);

// The preset (graphics.preset) and the levels ([graphics_custom]) of Scacelith.ini. Reading: a
// preset other than Custom gives its own levels whatever the file holds; a file from before the
// presets existed (graphics.quality 0..3 Low..Ultra, with the motion blur and depth of field
// switches) gives the matching preset, or Custom with that preset's levels when a switch turned
// off what the preset has on. Writing a preset other than Custom writes its own levels, and also
// the old keys, read by older versions of the game.
void readGraphicsSettings(const IniFile& ini, int& preset, GraphicsLevels& levels);
void writeGraphicsSettings(IniFile& ini, int preset, const GraphicsLevels& levels);

}  // namespace game
