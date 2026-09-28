// Internal to src/ai: the deterministic pieces of the opponent's behaviour (strength mapping,
// human-like thinking time, draw decisions), kept as free functions so they can be unit-tested.
#pragma once
#include "ai/engine.h"

#include <string>
#include <vector>

namespace ai::detail {

// Stockfish 16's Skill level for these settings (search.cpp, Skill::Skill): UCI_Elo is mapped to a
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

// True if `reply` (UCI) captures on the destination square of the last move of `moves` (played
// from the standard start position) and that last move was itself a capture.
bool isRecapture(const std::vector<std::string>& moves, const std::string& reply);

}  // namespace ai::detail
