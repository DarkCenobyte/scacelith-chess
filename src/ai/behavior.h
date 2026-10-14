// Internal to src/ai: the deterministic pieces of the opponent's behaviour (strength mapping,
// human-like thinking time, draw decisions), kept as free functions so they can be unit-tested.
#pragma once
#include "ai/engine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ai::detail {

// Stockfish's Skill level for these settings (search.h, Skill::Skill): UCI_Elo is mapped to a
// fractional level with Stockfish's own fit; -1 = full strength (no Skill handicap).
double skillLevel(const EngineSettings& s);

// Human-like thinking time (see Engine::thinkTimeMs). `z` is a standard normal sample (the
// log-normal jitter), `recapture` true when the chosen move recaptures on the square where the
// opponent just captured.
int humanThinkTimeMs(const EngineSettings& s, const ClockInfo& clock, int plyCount, int legalMoveCount,
                     bool inCheck, bool recapture, double z);

bool acceptsDrawOffer(int evalCp, int plyCount);
bool offersDraw(int evalCp, int plyCount, int pliesSinceOwnOffer);

// Same strength settings (everything that changes the moves Stockfish plays).
bool sameStrength(const EngineSettings& a, const EngineSettings& b);

// "Dynamic Elo by time" (ClockInfo::dynamicElo, presets.cpp): how many Elo the side to move loses
// to time pressure with `remainingMs` on its clock and `incMs` of increment, in a game of `baseMs`
// base time. 0 with enough time; up to kLowTimeEloDrop once its clock runs low, up to
// kTimeTroubleEloDrop in time trouble.
constexpr int kLowTimeEloDrop = 150;
constexpr int kTimeTroubleEloDrop = 400;
int timePressureEloDrop(int64_t remainingMs, int64_t incMs, int64_t baseMs);
// Settings that play `drop` Elo below `s` (the strength fields; threads, hash, humanize and clock
// use are kept), on the presets' scale: UCI_Elo from 1200 up, the depth-capped handicaps of the
// weak presets and coach levels below. `s` itself when drop <= 0, when nothing measured lies
// closer to the target than `s` does, or when the strength of `s` cannot be placed on that scale
// (a node or move time limit, a depth cap at full strength).
EngineSettings weakenedSettings(const EngineSettings& s, int drop);

// True if `reply` (UCI) captures on the destination square of the last move of `moves` (played
// from the standard start position) and that last move was itself a capture.
bool isRecapture(const std::vector<std::string>& moves, const std::string& reply);

}  // namespace ai::detail
