// The coach's review of the human's moves in a Coach game (levels 1-6; level 0 is the rules
// lesson): move classification from Stockfish figures (the lichess win-percentage model), the
// choice of an explanation backed by board facts (tactics.h), praise and takeback policy per level,
// rate limits, and the Script the director performs: lines with pointing and marks, demonstration
// moves played on the table and taken back, the takeback offer. Also the "Check!" / "Checkmate!"
// announcements (either side) and the coach's threat warnings to beginners.
//
// Specification: research-pedagogy.md §1 (bands, classification, praise, takebacks), §2 (the
// explanation taxonomy, demonstrations, sentence assembly), §2.20 (announcements), §6.3/§6.5
// (gesture density, priorities). Lines are catalog keys of assets/coach/speech/<lang>/review.lang;
// every pointing target is a placeholder of the line (the gesture's anchor), so translations keep
// the timing.
//
// Engine-free: the director (W9) runs the analyses this file asks for (Reviewer::*Request) and hands
// the results back as ai::Analysis values; tests build those by hand.
#pragma once
#include "../ai/analysis.h"
#include "../chess/chess.h"
#include "script.h"
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace coach {

// ---- Win percentage and move classes (research-pedagogy §1.3) ------------------------------------
// Lichess: W% = 50 + 50 * (2 / (1 + e^(-0.00368208 cp)) - 1), cp clamped to +-1000; mates count
// as +-1000 cp. Scores are the mover's view (Stockfish reports the side to move's).
double winPercent(int cp);
double winPercent(const ai::Score& s);
// Lichess per-move accuracy (with its +1 "uncertainty bonus"), 0..100; 100 when nothing was lost.
double moveAccuracy(double wBefore, double wAfter);

enum class MoveClass : uint8_t { Unjudged, Book, Forced, Best, Excellent, Good, Inaccuracy, Mistake, Blunder };
enum class MateChange : uint8_t {
    None,
    Created,   // the best line did not lose to a mate, the played one does
    Lost,      // the best line mated, the played one does not
    Delayed    // both mate, the played one later
};
const char* moveClassName(MoveClass c);   // "best", "blunder", ... (logs, tests)

struct Judgement {
    MoveClass cls = MoveClass::Unjudged;
    MateChange mate = MateChange::None;
    double wBest = 50.0, wPlayed = 50.0;   // mover's view, same root
    double delta = 0.0;                    // max(0, wBest - wPlayed), W% points
};
// Classes from Δ (Best < 0.5 or the engine's move, Excellent < 2, Good < 5, Inaccuracy < 10,
// Mistake < 15, Blunder >= 15) and the lichess mate rules (a created or lost mate is a blunder,
// softened when the other side of it was already clearly decided).
Judgement judge(const ai::Score& best, const ai::Score& played, bool playedIsBest);
MoveClass classifyDelta(double delta, bool playedIsBest);

// ---- Explanation types (research-pedagogy §2, in the order they are tried) -----------------------
enum class ExType : uint8_t {
    None,
    MateAllowed,    // §2.9
    MateMissed,     // §2.10
    Stalemate,      // §2.14
    Fork,           // §2.4
    Discovered,     // §2.7
    Skewer,         // §2.6
    Pin,            // §2.5
    Trapped,        // §2.12
    BackRank,       // §2.8
    Hanging,        // §2.1
    Exchange,       // §2.2
    MissedCapture,  // §2.3
    MissedFork,     // §2.4 (the human's fork not played)
    PromotionRace,  // §2.13
    KingSafety,     // §2.11
    BadTrade,       // §2.15
    Opening,        // §2.16
    Endgame,        // §2.17
    Positional      // §2.19
};
const char* exTypeName(ExType t);   // "fork", "hanging", ...: key part of ex.<name>.b<n> and theme.<name>

// ---- Per-ply verdicts (research-pedagogy §5.1), collected for the end-of-game appraisal ---------
struct PlyVerdict {
    int ply = -1;                    // index in Game::moves() (0 = White's first move)
    chess::Color mover = chess::White;
    bool human = false;              // the listener's move
    std::string uci, san;
    MoveClass cls = MoveClass::Unjudged;
    double wBest = 50.0, wPlayed = 50.0;   // mover's view, W%
    double delta = 0.0;
    double accuracy = 100.0;         // moveAccuracy(wBest, wPlayed)
    int cpWhiteAfter = 0;            // evaluation after the move, White's view, mates +-1000
    bool hasEvalAfter = false;
    int cpWhiteBefore = 0;           // evaluation of the position before the move (best line), same scale
    bool hasEvalBefore = false;
    std::string bestSan, bestUci;    // the engine's move
    uint8_t phase = 0;               // 0 opening, 1 middlegame, 2 endgame
    ExType exType = ExType::None;    // the explanation chosen (voiced or not)
    int materialSwing = 0;           // points won (+) or lost (-) along the played line within 3 plies
    int captured = 0;                // points this move captured
    bool check = false;
    bool only = false, brilliant = false, great = false, goodCapture = false;
    bool mateMissed = false, mateAllowed = false;
    bool voiced = false, offered = false, praised = false;
    bool coachHungPiece = false;     // coach move: left a piece of 3+ points hanging (for "did you see it?")
    chess::PieceType hungType = chess::NoPiece;
    chess::Square hungSquare = chess::NoSquare;
};

struct TakebackRecord {
    int ply = -1;
    std::string firstUci, firstSan;  // the move taken back
    MoveClass firstClass = MoveClass::Unjudged;
    ExType firstType = ExType::None;
    bool fixed = false;              // the replacement lost less than 5 W% points
    bool same = false;               // the same move was played again
};

// ---- Level parameters (design §1, research-pedagogy §1.4) ---------------------------------------
struct Band {
    int level = 1;
    int demoPlies = 1;          // deepest demonstration moved on the table
    int mateLinePlies = 1;      // longest mating line shown (M1 = 1 ply, M2 = 3, M3 = 5, M4 = 7)
    int missedMateMax = 1;      // missed mates voiced up to this many moves
    int lookahead = 2;          // plies of the refutation searched for a concrete cause
    int sentences = 3;          // Say lines per explanation (demo narration, rewind and offer excluded)
    int remarksPer10 = 5;       // unsolicited remarks (praise, tips, mistakes) per 10 human moves
    int praiseEvery = 3;        // at most one praise per this many human moves
    int offerCap = -1;          // takeback offers per game (-1: no cap)
    float demoPause = 0.8f;     // pause after a demonstration move lands (research-pedagogy §6.2)
};
Band band(int level);           // clamped to 1..6

// ---- The review --------------------------------------------------------------------------------
struct ReviewInput {
    const chess::Game* game = nullptr;       // the human's move is the last one played
    const ai::Analysis* before = nullptr;    // A0: MultiPV 3 on the position before the move
    const ai::Analysis* played = nullptr;    // A1: the played move re-scored at the same root (when A0 lacks it)
    const ai::Analysis* after = nullptr;     // A2 (optional): the position after the move, for a longer refutation
    const ai::Analysis* shallow = nullptr;   // A3 (optional): a shallow search of the same root ("tricky" praise)
    bool inBook = false;                     // the position after the move is in the openings book (W10)
};

struct Review {
    Script script;                // empty when the coach says nothing
    PlyVerdict verdict;           // store it for the appraisal (Appraisal::addReview)
    bool offersTakeback = false;  // the script ends with an OfferTakeback beat
    bool isRetry = false;         // this move replaced a taken-back one: 'takeback' describes it
    TakebackRecord takeback;
};

class Reviewer {
public:
    // A new Coach game. offersEnabled false: judge and explain, never offer (engine or mode limits).
    void reset(int level, chess::Color human, bool offersEnabled = true);
    int level() const { return level_; }
    chess::Color human() const { return human_; }

    // Analyses for a review (full strength; the coach's play settings do not apply):
    // A0 when the human's turn begins (MultiPV 3 on the current position)...
    ai::AnalysisRequest beforeRequest(const chess::Game& g) const;
    // ...A1 once the human's move is on the Game, only when A0 does not hold the played move
    // (same root, same depth, searchmoves = the move)...
    static bool needsPlayedRequest(const ai::Analysis& before, const std::string& playedUci);
    ai::AnalysisRequest playedRequest(const chess::Game& g, const ai::Analysis& before) const;
    // ...and A2 (optional) on the position after the move, for a refutation longer than A0's PV.
    ai::AnalysisRequest afterRequest(const chess::Game& g) const;

    // Right after any move is completed (either side): "Check!", "Checkmate!" (Urgent beats).
    Script announce(const chess::Game& g);

    // The human's move (the last of the Game) judged and explained. Call once per human move, after
    // announce(). Unjudged when A0 is missing or failed (engine unavailable): rules-based only.
    Review review(const ReviewInput& in);

    // After the coach's own move, before the human plays: warnings about the coach's new threats
    // (levels 1-2) and, at level 1, "my last move was a mistake" once A0 of the human's turn
    // ('before', optional) confirms the capture. Low-priority beats.
    Script coachMoved(const chess::Game& g, const ai::Analysis* before = nullptr);

    // The player's answer to an offer. Accepted: call after Game::undo(1); returns the hint
    // (levels 1-2) and remembers the move so the replay is judged against it. Declined: nothing
    // is said by the review (the director says the events line) and nothing is remembered.
    Script takebackAccepted(const chess::Game& g);
    void takebackDeclined();

    int offersMade() const { return offers_; }
    int humanMoves() const { return humanMoves_; }

private:
    int level_ = 1;
    chess::Color human_ = chess::White;
    bool offersEnabled_ = true;
    int humanMoves_ = 0;              // human moves reviewed (retries excluded)
    int lastPraise_ = -1000;          // humanMoves_ at the last praise
    std::deque<int> remarks_;         // humanMoves_ of each unsolicited remark (sliding window of 10)
    int offers_ = 0;
    bool brilliantDone_ = false;
    int bookPraises_ = 0;
    bool castlePraised_ = false;
    bool coachCheckExplained_ = false;
    bool punishHintDone_ = false;
    uint32_t tipsSaid_ = 0;           // opening principles and endgame tips already voiced (bits)
    double lastHumanWPlayed_ = -1.0;  // the human's W% after their previous move (their view)
    // The last offer, and a takeback in progress.
    TakebackRecord offered_;          // the move the last offer was about
    chess::Square offeredHint_ = chess::NoSquare;
    int offersAtPly_ = 0;             // offers made for offered_.ply
    bool retryPending_ = false;
    TakebackRecord retry_;

    bool remarkAllowed() const;
    void noteRemark();
};

}  // namespace coach
