// Helpers shared by the translation units of GameScene (game_scene*.cpp), and only by them: one
// copy, so that solo, online, coach and hot-seat games never drift apart.
#pragma once
#include "../anim/animator.h"
#include "../coach/rewind.h"
#include "../i18n/i18n.h"
#include "../math/math.h"
#include "settings.h"
#include <cstdlib>
#include <string>

namespace game {
namespace scene_detail {

constexpr float kGlanceTime = 0.45f;   // seconds to turn to the scoresheet and back
constexpr float kFov = 52.0f * m::DEG; // the view's vertical field of view (eyes, observer)

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

// The hand whose half holds an off-board spot of a trip taken back (pos.z > 0 is White's, seat
// 0), or 'fallback' (a trip from square to square: the hand of the player who made the move).
// The coach's takebacks and the Analysis mode's steps back.
inline int handForTrip(const coach::PieceTrip& t, int fallback) {
    if (t.from.kind != coach::RestKind::Square) return t.from.pos.z > 0.0f ? 0 : 1;
    if (t.to.kind != coach::RestKind::Square) return t.to.pos.z > 0.0f ? 0 : 1;
    return fallback;
}

// A rating change as the end card shows it: "+12", "−12", "±0".
inline std::string signedDelta(int d) {
    return (d > 0 ? "+" : d < 0 ? "\xE2\x88\x92" : "\xC2\xB1") + std::to_string(std::abs(d));
}

}  // namespace scene_detail
}  // namespace game
