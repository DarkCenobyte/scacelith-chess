// Human-like behaviour of the opponent: how long it "thinks" before moving and when it accepts a
// draw. Pure functions (see behavior.h) so the numbers below can be unit-tested.
#include "ai/behavior.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ai::detail {
namespace {

double lerp(double a, double b, double t) { return a + (b - a) * std::clamp(t, 0.0, 1.0); }

// Share of a normal thinking time spent at this point of the game (move number of the AI).
double phaseFactor(int moveNo) {
    if (moveNo <= 3) return 0.25;                              // opening moves everyone knows
    if (moveNo <= 10) return lerp(0.35, 1.0, (moveNo - 4) / 6.0); // leaving "book"
    if (moveNo <= 40) return 1.1;                              // middlegame: the real thinking
    return 0.9;                                                // endgame: fewer pieces, technique
}

}  // namespace

int humanThinkTimeMs(const EngineSettings& s, const ClockInfo& clock, int plyCount, int legalMoveCount,
                     bool inCheck, bool recapture, double z) {
    if (!s.humanize) return 0;
    // 0 = novice .. 1 = grandmaster and above. Stronger players take their time; weak ones play
    // quickly (and it shows).
    const double strength = std::clamp((Engine::estimateElo(s) - 800) / 1900.0, 0.0, 1.0);
    const int moveNo = std::max(0, plyCount) / 2 + 1;
    // Log-normal jitter (median 1, sigma 0.45): mostly 0.5x..2x, occasionally a long think.
    const double jitter = std::exp(0.45 * std::clamp(z, -2.5, 2.5));

    // How forced the move is: an only move, a recapture or a check evasion needs little thought.
    double forced = 1.0;
    if (legalMoveCount == 1) forced = 0.12;
    else {
        if (recapture) forced = std::min(forced, 0.3);
        if (inCheck) forced = std::min(forced, 0.6);
        if (legalMoveCount > 0 && legalMoveCount <= 3) forced = std::min(forced, 0.7);
    }

    // Median ("target") time for this move, then a log-normal spread above a floor:
    //   t = floor + (target - floor) * jitter
    // so times never pile up at a clamp (which would look mechanical) and the median stays target.
    const double floorMs = 600.0;  // look, decide, reach: nobody plays faster over the board
    auto spread = [&](double target) { return floorMs + std::max(target - floorMs, 250.0) * jitter; };

    if (!clock.timed) {
        // Untimed: a normal middlegame move takes a pleasant few seconds, median ~2.7 s for the
        // Novice up to ~7 s at full strength (typically 2..12 s); less in the opening and for
        // forced moves.
        double t = spread((2500.0 + 4000.0 * strength) * phaseFactor(moveNo) * forced);
        if (legalMoveCount == 1) t = std::min(t, 1500.0);
        return int(std::clamp(t, floorMs, 12000.0));
    }

    const bool white = (std::max(0, plyCount) % 2) == 0;  // the AI is the side to move
    const double remaining = double(std::max<int64_t>(0, white ? clock.whiteMs : clock.blackMs));
    const double inc = double(std::max<int64_t>(0, white ? clock.whiteIncMs : clock.blackIncMs));
    const double overhead = double(std::max(0, clock.moveOverheadMs));

    // Per-move budget as a human would split the clock: remaining/35 + increment. Weaker players
    // use less of it.
    const double budget = remaining / 35.0 + inc;
    double t = spread(budget * (0.6 + 0.4 * strength) * phaseFactor(moveNo) * forced);
    if (legalMoveCount == 1) t = std::min(t, 1500.0);

    // Never flag: this move's own overhead and a 0.5 s safety margin are untouchable; of the rest,
    // spend at most 10% (plus most of the increment, which comes back) and never more than half.
    // In real time trouble that drops below the floor: the AI then plays almost instantly.
    const double usable = remaining - overhead - 500.0;
    if (usable <= 0.0) return 0;
    const double cap = std::min(usable * 0.10 + inc * 0.8, usable * 0.5);
    return int(std::clamp(t, std::min(floorMs, cap), cap));
}

// Draw offers / claims. evalCp is the engine's evaluation from the AI's point of view.
//   eval >= +80 cp         decline: the AI is better and plays on
//   eval <= -80 cp         accept at any time: half a point is welcome when worse
//   otherwise (roughly equal):
//     before move 15       decline: too early, a real player wants a game
//     eval <= -30 cp       accept: slightly worse and out of the opening
//     |eval| <= 25 cp      accept from move 30 on: dead equal, nothing left to play for
//     else                 decline
bool acceptsDrawOffer(int evalCp, int plyCount) {
    if (evalCp >= 80) return false;
    if (evalCp <= -80) return true;
    if (plyCount < 30) return false;
    if (evalCp <= -30) return true;
    return std::abs(evalCp) <= 25 && plyCount >= 60;
}

namespace {

// Just enough of a board to tell captures apart: piece letters (FEN case), '.' = empty.
struct MiniBoard {
    char sq[64];
    MiniBoard() {
        const char* back = "RNBQKBNR";
        for (int f = 0; f < 8; ++f) {
            sq[f] = back[f];
            sq[8 + f] = 'P';
            sq[48 + f] = 'p';
            sq[56 + f] = char(back[f] - 'A' + 'a');
            for (int r = 2; r < 6; ++r) sq[r * 8 + f] = '.';
        }
    }
    static int parse(const std::string& m, size_t at) {
        if (m.size() < at + 2) return -1;
        int f = m[at] - 'a', r = m[at + 1] - '1';
        return (f >= 0 && f < 8 && r >= 0 && r < 8) ? r * 8 + f : -1;
    }
    // Plays a UCI move, returns true if it captured something (en passant included).
    bool play(const std::string& m) {
        int from = parse(m, 0), to = parse(m, 2);
        if (from < 0 || to < 0) return false;
        char pc = sq[from];
        bool capture = sq[to] != '.';
        const bool pawn = pc == 'P' || pc == 'p', king = pc == 'K' || pc == 'k';
        if (pawn && (from & 7) != (to & 7) && sq[to] == '.') {  // en passant
            sq[(from & ~7) | (to & 7)] = '.';
            capture = true;
        }
        if (king && std::abs((to & 7) - (from & 7)) == 2) {  // castling: move the rook too
            int rank = from & ~7;
            int rookFrom = rank + ((to & 7) == 6 ? 7 : 0), rookTo = rank + ((to & 7) == 6 ? 5 : 3);
            sq[rookTo] = sq[rookFrom];
            sq[rookFrom] = '.';
        }
        if (m.size() >= 5 && pawn) {
            char p = m[4];
            pc = (pc == 'P') ? char(p - 'a' + 'A') : p;
        }
        sq[to] = pc;
        sq[from] = '.';
        return capture;
    }
};

}  // namespace

bool isRecapture(const std::vector<std::string>& moves, const std::string& reply) {
    if (moves.empty() || reply.size() < 4) return false;
    MiniBoard b;
    bool lastCapture = false;
    for (const auto& m : moves) lastCapture = b.play(m);
    const std::string& last = moves.back();
    return lastCapture && last.size() >= 4 && reply.compare(2, 2, last, 2, 2) == 0;
}

}  // namespace ai::detail
