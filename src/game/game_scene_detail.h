// Helpers shared by the translation units of GameScene (game_scene*.cpp), and only by them: one
// copy, so that solo, online, coach and hot-seat games never drift apart.
#pragma once
#include "../anim/animator.h"
#include "../i18n/i18n.h"
#include "../math/math.h"
#include "settings.h"
#include <cstdlib>
#include <string>

namespace game {
namespace scene_detail {

constexpr float kGlanceTime = 0.45f;   // seconds to turn to the scoresheet and back

inline anim::Task task(anim::TaskType t, int pieceId = -1, m::vec3 pos = m::vec3(0), float height = 0.0f,
                       float duration = 0.0f) {
    anim::Task k;
    k.type = t;
    k.pieceId = pieceId;
    k.position = pos;
    k.height = height;
    k.duration = duration;
    return k;
}

// The human's name on the scoresheets (Options > Player; "Human" by default, written in the
// interface language).
inline std::string localPlayerName() {
    const std::string& n = settings().playerName;
    return n.empty() || n == "Human" ? std::string(i18n::tr("player.default_name")) : n;
}

// A rating change as the end card shows it: "+12", "−12", "±0".
inline std::string signedDelta(int d) {
    return (d > 0 ? "+" : d < 0 ? "\xE2\x88\x92" : "\xC2\xB1") + std::to_string(std::abs(d));
}

}  // namespace scene_detail
}  // namespace game
