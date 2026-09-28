// Scacelith UI palette and type scale: ivory text, old gold hairlines, black velvet panels and a
// burgundy velvet accent for primary actions. All colours are display (sRGB) values.
#pragma once
#include "../math/math.h"
#include "ui_font.h"

namespace ui {
namespace theme {
using m::vec4;

constexpr vec4 ivory{0.925f, 0.894f, 0.827f, 1.0f};
constexpr vec4 ivoryDim{0.760f, 0.722f, 0.651f, 1.0f};
constexpr vec4 muted{0.560f, 0.522f, 0.463f, 1.0f};
constexpr vec4 faint{0.420f, 0.392f, 0.353f, 1.0f};
constexpr vec4 gold{0.788f, 0.659f, 0.420f, 1.0f};
constexpr vec4 goldBright{0.925f, 0.816f, 0.588f, 1.0f};
constexpr vec4 goldDeep{0.478f, 0.380f, 0.212f, 1.0f};
constexpr vec4 velvet{0.330f, 0.055f, 0.078f, 1.0f};
constexpr vec4 velvetDeep{0.180f, 0.027f, 0.043f, 1.0f};
constexpr vec4 velvetBright{0.470f, 0.090f, 0.118f, 1.0f};
constexpr vec4 panelTop{0.060f, 0.052f, 0.047f, 0.90f};
constexpr vec4 panelBottom{0.030f, 0.026f, 0.024f, 0.93f};
constexpr vec4 black{0.0f, 0.0f, 0.0f, 1.0f};
constexpr vec4 danger{0.780f, 0.360f, 0.300f, 1.0f};

inline vec4 withAlpha(vec4 c, float a) { return {c.x, c.y, c.z, c.w * a}; }
inline vec4 mix(vec4 a, vec4 b, float t) { return a + (b - a) * t; }

// Type scale (reference pixels at 1080p).
constexpr float kWordmark = 118.0f;
constexpr float kPageTitle = 40.0f;
constexpr float kSection = 21.0f;
constexpr float kMenuEntry = 33.0f;
constexpr float kButton = 22.0f;
constexpr float kBody = 27.0f;
constexpr float kSmall = 22.0f;
constexpr float kCaption = 20.0f;

constexpr float kTrackTitle = 0.16f;   // letter spacing of Cinzel labels (em)
constexpr float kTrackWordmark = 0.22f;

}  // namespace theme
}  // namespace ui
